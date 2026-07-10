// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_fx.cpp — track & take FX: add/list/remove, param get/set, enable/bypass, and presets. Also
// the substrate the spatial layer drives to wire IEM/SPARTA/ambiX/ATK encoders & decoders.
//
// Params are exposed both raw (plugin units, with min/max) and normalized [0,1] — set either.
// Dual-path; mutations single-undo.

#include <string>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK
namespace {
std::string fxName(MediaTrack* t, int fx) {
    char buf[256] = {0};
    TrackFX_GetFXName(t, fx, buf, sizeof(buf));
    return buf;
}
std::string fxParamName(MediaTrack* t, int fx, int p) {
    char buf[256] = {0};
    TrackFX_GetParamName(t, fx, p, buf, sizeof(buf));
    return buf;
}
// ---- take-FX name helpers, mirroring the track-FX ones above ----
std::string takeFxName(MediaItem_Take* tk, int fx) {
    char buf[256] = {0};
    TakeFX_GetFXName(tk, fx, buf, sizeof(buf));
    return buf;
}
std::string takeFxParamName(MediaItem_Take* tk, int fx, int p) {
    char buf[256] = {0};
    TakeFX_GetParamName(tk, fx, p, buf, sizeof(buf));
    return buf;
}
}  // namespace
#endif

void registerFxTools(ToolRegistry& reg) {
    // ---- fx.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "fx.add", "Add an FX by name to a track's FX chain (always instantiates a new instance). "
                  "Name matches REAPER's Add-FX search (e.g. 'ReaEQ', 'VST3:Pro-Q 3', 'IEM StereoEncoder').",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "name":{"type":"string"}},"required":["track","name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxIndex":{"type":"integer"},"name":{"type":"string"},
            "fxCount":{"type":"integer"}},"required":["fxIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "name");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            int fx = TrackFX_AddByName(t, name.c_str(), false, -1);  // -1 = always create new
            Undo_EndBlock2(kCur, "MCP: add FX", -1);
            if (fx < 0) throw std::runtime_error("FX not found / could not add: " + name);
            return Json{{"fxIndex", fx}, {"name", fxName(t, fx)}, {"fxCount", TrackFX_GetCount(t)}};
#else
            (void)idx;
            return Json{{"fxIndex", 0}, {"name", name}, {"fxCount", 1}};
#endif
        }});

    // ---- fx.list (read-only) ----
    reg.add(Tool{
        "fx.list", "List a track's FX chain: index, name, parameter count, enabled state.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"fxCount":{"type":"integer"},
            "fx":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},
                "name":{"type":"string"},"numParams":{"type":"integer"},"enabled":{"type":"boolean"}}}}},
            "required":["track","fxCount","fx"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int n = TrackFX_GetCount(t);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i)
                arr.push_back(Json{{"index", i}, {"name", fxName(t, i)},
                                   {"numParams", TrackFX_GetNumParams(t, i)},
                                   {"enabled", TrackFX_GetEnabled(t, i)}});
            return Json{{"track", idx}, {"fxCount", n}, {"fx", std::move(arr)}};
#else
            return Json{{"track", idx}, {"fxCount", 0}, {"fx", Json::array()}};
#endif
        }});

    // ---- fx.get_param (read-only) ----
    reg.add(Tool{
        "fx.get_param", "Read one FX parameter: raw value + min/max (plugin units) and normalized [0,1].",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0},"param":{"type":"integer","minimum":0}},
            "required":["track","fx","param"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"name":{"type":"string"},"value":{"type":"number"},
            "normalized":{"type":"number"},"min":{"type":"number"},"max":{"type":"number"}},
            "required":["value","normalized"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx"), p = reqInt(a, "param");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            double mn = 0.0, mx = 0.0;
            double val = TrackFX_GetParam(t, fx, p, &mn, &mx);
            return Json{{"name", fxParamName(t, fx, p)}, {"value", val},
                        {"normalized", TrackFX_GetParamNormalized(t, fx, p)}, {"min", mn}, {"max", mx}};
#else
            (void)idx; (void)fx; (void)p;
            return Json{{"name", ""}, {"value", 0.0}, {"normalized", 0.0}, {"min", 0.0}, {"max", 1.0}};
#endif
        }});

    // ---- fx.set_param (mutating; undo-wrapped) ----
    reg.add(Tool{
        "fx.set_param", "Set one FX parameter. Provide 'normalized' [0,1] OR 'value' (raw plugin units).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0},"param":{"type":"integer","minimum":0},
            "value":{"type":"number"},"normalized":{"type":"number","minimum":0,"maximum":1}},
            "required":["track","fx","param"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"value":{"type":"number"},
            "normalized":{"type":"number"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx"), p = reqInt(a, "param");
            const bool hasNorm = a.contains("normalized") && a["normalized"].is_number();
            const bool hasVal = a.contains("value") && a["value"].is_number();
            if (!hasNorm && !hasVal)
                throw std::runtime_error("fx.set_param requires 'normalized' or 'value'");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (hasNorm) TrackFX_SetParamNormalized(t, fx, p, a["normalized"].get<double>());
            else         TrackFX_SetParam(t, fx, p, a["value"].get<double>());
            Undo_EndBlock2(kCur, "MCP: set FX param", -1);
            double mn = 0.0, mx = 0.0;
            return Json{{"ok", true}, {"value", TrackFX_GetParam(t, fx, p, &mn, &mx)},
                        {"normalized", TrackFX_GetParamNormalized(t, fx, p)}};
#else
            (void)idx; (void)fx; (void)p;
            return Json{{"ok", true},
                        {"value", hasVal ? a["value"].get<double>() : 0.0},
                        {"normalized", hasNorm ? a["normalized"].get<double>() : 0.0}};
#endif
        }});

    // ---- fx.remove (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "fx.remove", "Remove an FX from a track's chain by index. Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0}},"required":["track","fx"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "removedIndex":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            bool ok = TrackFX_Delete(t, fx);
            Undo_EndBlock2(kCur, "MCP: remove FX", -1);
            return Json{{"ok", ok}, {"removedIndex", fx}};
#else
            (void)idx;
            return Json{{"ok", true}, {"removedIndex", fx}};
#endif
        }});

    // ================================ FX depth ================================

    // ---- fx.set_enabled (mutating; undo-wrapped, idempotent) ----
    reg.add(Tool{
        "fx.set_enabled", "Enable or bypass a track FX by index. enabled:false = bypassed (the REAPER "
                          "'FX bypass' state). Idempotent; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0},"enabled":{"type":"boolean"}},
            "required":["track","fx","enabled"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"enabled":{"type":"boolean"}},
            "required":["ok","enabled"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx");
            const bool enabled = reqBool(a, "enabled");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            TrackFX_SetEnabled(t, fx, enabled);
            Undo_EndBlock2(kCur, "MCP: set FX enabled", -1);
            return Json{{"ok", true}, {"enabled", TrackFX_GetEnabled(t, fx)}};
#else
            (void)idx; (void)fx;
            return Json{{"ok", true}, {"enabled", enabled}};
#endif
        }});

    // ---- fx.get_preset (read-only) ----
    reg.add(Tool{
        "fx.get_preset", "Read a track FX's current preset: name, 0-based index, and total preset count "
                         "(index -1 when no named preset is active).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0}},"required":["track","fx"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"preset":{"type":"string"},
            "presetIndex":{"type":"integer"},"presetCount":{"type":"integer"}},
            "required":["preset","presetIndex","presetCount"]})"),
        ToolAnnotations{/*readOnly*/ true, false, /*idempotent*/ true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            char nm[512] = {0};
            TrackFX_GetPreset(t, fx, nm, sizeof(nm));
            int total = 0;
            const int pi = TrackFX_GetPresetIndex(t, fx, &total);
            return Json{{"preset", std::string(nm)}, {"presetIndex", pi}, {"presetCount", total}};
#else
            (void)idx; (void)fx;
            return Json{{"preset", ""}, {"presetIndex", -1}, {"presetCount", 0}};
#endif
        }});

    // ---- fx.set_preset (mutating; undo-wrapped) ----
    reg.add(Tool{
        "fx.set_preset", "Activate a track FX preset. Provide 'preset' (the exact name shown in REAPER's "
                         "preset dropdown, or a .vstpreset path for VST3) OR 'move' (+N / -N to step "
                         "through presets). Echoes the resulting preset name.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0},"preset":{"type":"string"},"move":{"type":"integer"}},
            "required":["track","fx"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"preset":{"type":"string"},
            "presetIndex":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track"), fx = reqInt(a, "fx");
            const bool hasPreset = a.contains("preset") && a["preset"].is_string();
            const bool hasMove = a.contains("move") && a["move"].is_number();
            if (!hasPreset && !hasMove)
                throw std::runtime_error("fx.set_preset requires 'preset' (name) or 'move' (+/- steps)");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            bool ok = false;
            if (hasPreset) ok = TrackFX_SetPreset(t, fx, a["preset"].get<std::string>().c_str());
            else           ok = TrackFX_NavigatePresets(t, fx, a["move"].get<int>());
            Undo_EndBlock2(kCur, "MCP: set FX preset", -1);
            char nm[512] = {0};
            TrackFX_GetPreset(t, fx, nm, sizeof(nm));
            int total = 0;
            return Json{{"ok", ok}, {"preset", std::string(nm)},
                        {"presetIndex", TrackFX_GetPresetIndex(t, fx, &total)}};
#else
            (void)idx; (void)fx;
            return Json{{"ok", true}, {"preset", hasPreset ? a["preset"].get<std::string>() : ""},
                        {"presetIndex", -1}};
#endif
        }});

    // ============================== Take FX chain ==============================
    // Take FX live on a specific take of a media item. An item is addressed by (track, item); 'take'
    // is optional (default -1 = the item's active take). This mirrors the track-FX family above.

    // ---- takefx.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "takefx.add", "Add an FX by name to a take's FX chain (always a new instance). Name matches "
                      "REAPER's Add-FX search. 'take' defaults to the item's active take.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"},"name":{"type":"string"}},
            "required":["track","item","name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxIndex":{"type":"integer"},"name":{"type":"string"},
            "fxCount":{"type":"integer"}},"required":["fxIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const std::string name = reqStr(a, "name");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            Undo_BeginBlock2(kCur);
            int fx = TakeFX_AddByName(tk, name.c_str(), -1);  // -1 = always create new
            Undo_EndBlock2(kCur, "MCP: add take FX", -1);
            if (fx < 0) throw std::runtime_error("FX not found / could not add: " + name);
            return Json{{"fxIndex", fx}, {"name", takeFxName(tk, fx)}, {"fxCount", TakeFX_GetCount(tk)}};
#else
            (void)tr; (void)it; (void)takeIdx;
            return Json{{"fxIndex", 0}, {"name", name}, {"fxCount", 1}};
#endif
        }});

    // ---- takefx.list (read-only) ----
    reg.add(Tool{
        "takefx.list", "List a take's FX chain: index, name, parameter count, enabled state.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxCount":{"type":"integer"},
            "fx":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},
                "name":{"type":"string"},"numParams":{"type":"integer"},"enabled":{"type":"boolean"}}}}},
            "required":["fxCount","fx"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            const int n = TakeFX_GetCount(tk);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i)
                arr.push_back(Json{{"index", i}, {"name", takeFxName(tk, i)},
                                   {"numParams", TakeFX_GetNumParams(tk, i)},
                                   {"enabled", TakeFX_GetEnabled(tk, i)}});
            return Json{{"fxCount", n}, {"fx", std::move(arr)}};
#else
            (void)tr; (void)it; (void)takeIdx;
            return Json{{"fxCount", 0}, {"fx", Json::array()}};
#endif
        }});

    // ---- takefx.remove (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "takefx.remove", "Remove an FX from a take's chain by index. Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"},
            "fx":{"type":"integer","minimum":0}},"required":["track","item","fx"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "removedIndex":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int fx = reqInt(a, "fx");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            Undo_BeginBlock2(kCur);
            bool ok = TakeFX_Delete(tk, fx);
            Undo_EndBlock2(kCur, "MCP: remove take FX", -1);
            return Json{{"ok", ok}, {"removedIndex", fx}};
#else
            (void)tr; (void)it; (void)takeIdx;
            return Json{{"ok", true}, {"removedIndex", fx}};
#endif
        }});

    // ---- takefx.get_param (read-only) ----
    reg.add(Tool{
        "takefx.get_param", "Read one take-FX parameter: raw value + min/max and normalized [0,1].",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"},
            "fx":{"type":"integer","minimum":0},"param":{"type":"integer","minimum":0}},
            "required":["track","item","fx","param"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"name":{"type":"string"},"value":{"type":"number"},
            "normalized":{"type":"number"},"min":{"type":"number"},"max":{"type":"number"}},
            "required":["value","normalized"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int fx = reqInt(a, "fx"), p = reqInt(a, "param");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            double mn = 0.0, mx = 0.0;
            double val = TakeFX_GetParam(tk, fx, p, &mn, &mx);
            return Json{{"name", takeFxParamName(tk, fx, p)}, {"value", val},
                        {"normalized", TakeFX_GetParamNormalized(tk, fx, p)}, {"min", mn}, {"max", mx}};
#else
            (void)tr; (void)it; (void)takeIdx; (void)fx; (void)p;
            return Json{{"name", ""}, {"value", 0.0}, {"normalized", 0.0}, {"min", 0.0}, {"max", 1.0}};
#endif
        }});

    // ---- takefx.set_param (mutating; undo-wrapped) ----
    reg.add(Tool{
        "takefx.set_param", "Set one take-FX parameter. Provide 'normalized' [0,1] OR 'value' (raw units).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"},
            "fx":{"type":"integer","minimum":0},"param":{"type":"integer","minimum":0},
            "value":{"type":"number"},"normalized":{"type":"number","minimum":0,"maximum":1}},
            "required":["track","item","fx","param"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"value":{"type":"number"},
            "normalized":{"type":"number"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int fx = reqInt(a, "fx"), p = reqInt(a, "param");
            const bool hasNorm = a.contains("normalized") && a["normalized"].is_number();
            const bool hasVal = a.contains("value") && a["value"].is_number();
            if (!hasNorm && !hasVal)
                throw std::runtime_error("takefx.set_param requires 'normalized' or 'value'");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            Undo_BeginBlock2(kCur);
            if (hasNorm) TakeFX_SetParamNormalized(tk, fx, p, a["normalized"].get<double>());
            else         TakeFX_SetParam(tk, fx, p, a["value"].get<double>());
            Undo_EndBlock2(kCur, "MCP: set take FX param", -1);
            double mn = 0.0, mx = 0.0;
            return Json{{"ok", true}, {"value", TakeFX_GetParam(tk, fx, p, &mn, &mx)},
                        {"normalized", TakeFX_GetParamNormalized(tk, fx, p)}};
#else
            (void)tr; (void)it; (void)takeIdx; (void)fx; (void)p;
            return Json{{"ok", true},
                        {"value", hasVal ? a["value"].get<double>() : 0.0},
                        {"normalized", hasNorm ? a["normalized"].get<double>() : 0.0}};
#endif
        }});

    // ---- takefx.set_enabled (mutating; idempotent) ----
    reg.add(Tool{
        "takefx.set_enabled", "Enable or bypass a take FX by index. Idempotent; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer"},
            "fx":{"type":"integer","minimum":0},"enabled":{"type":"boolean"}},
            "required":["track","item","fx","enabled"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"enabled":{"type":"boolean"}},
            "required":["ok","enabled"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), it = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int fx = reqInt(a, "fx");
            const bool enabled = reqBool(a, "enabled");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem_Take* tk = requireTake(requireItem(tr, it), takeIdx);
            Undo_BeginBlock2(kCur);
            TakeFX_SetEnabled(tk, fx, enabled);
            Undo_EndBlock2(kCur, "MCP: set take FX enabled", -1);
            return Json{{"ok", true}, {"enabled", TakeFX_GetEnabled(tk, fx)}};
#else
            (void)tr; (void)it; (void)takeIdx; (void)fx;
            return Json{{"ok", true}, {"enabled", enabled}};
#endif
        }});
}

}  // namespace reaper_mcp
