// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_core.cpp — core tool surface: transport, project (save/open/render), tracks (add/list,
// per-field setters, faders, freeze/unfreeze, metering), track state-chunk write/patch, undo/redo,
// and selection.
//
// Each handler runs on REAPER's MAIN THREAD (the MCP server submits it via MainThreadQueue), so it
// may call the REAPER API directly. Mutating tools wrap their work in Undo_BeginBlock2/EndBlock2 and
// return structured JSON matching their outputSchema.
//
// Dual-path: when built into the extension (REAPER_MCP_HAVE_SDK) the bodies call the native API;
// when built into the host test core (no SDK) they return representative, schema-valid structured
// output that echoes the arguments, so the transport + schemas can be exercised off a running REAPER.

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../reaper_api.h"     // no-op unless REAPER_MCP_HAVE_SDK
#include "../tool_registry.h"
#include "../composite_support.h"  // makeError — structured, agent-actionable guards for the project verbs
#include "../track_chunk.h"        // validateTrackChunk + diffTrackChunks (SDK-free)

namespace reaper_mcp {
namespace {

// ---- argument helpers (used by both paths) ----
int reqInt(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_number())
        throw std::runtime_error(std::string("missing/invalid integer arg: ") + k);
    return a[k].get<int>();
}
std::string reqStr(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_string())
        throw std::runtime_error(std::string("missing/invalid string arg: ") + k);
    return a[k].get<std::string>();
}
double optNum(const Json& a, const char* k, double d) {
    return (a.contains(k) && a[k].is_number()) ? a[k].get<double>() : d;
}
bool optBool(const Json& a, const char* k, bool d) {
    return (a.contains(k) && a[k].is_boolean()) ? a[k].get<bool>() : d;
}
double reqNum(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_number())
        throw std::runtime_error(std::string("missing/invalid number arg: ") + k);
    return a[k].get<double>();
}
bool reqBool(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_boolean())
        throw std::runtime_error(std::string("missing/invalid boolean arg: ") + k);
    return a[k].get<bool>();
}
int optInt(const Json& a, const char* k, int d) {
    return (a.contains(k) && a[k].is_number()) ? a[k].get<int>() : d;
}
std::string optStr(const Json& a, const char* k, const std::string& d) {
    return (a.contains(k) && a[k].is_string()) ? a[k].get<std::string>() : d;
}

#ifdef REAPER_MCP_HAVE_SDK
constexpr ReaProject* kCur = nullptr;  // REAPER treats a null ReaProject* as "current project"

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double gainToDb(double g) { return g > 0.0 ? 20.0 * std::log10(g) : -150.0; }

std::string trackGuid(MediaTrack* t) {
    char buf[64] = {0};
    if (t) guidToString(GetTrackGUID(t), buf);
    return buf;
}
std::string trackName(MediaTrack* t) {
    char buf[1024] = {0};
    if (t) GetSetMediaTrackInfo_String(t, "P_NAME", buf, false);
    return buf;
}
MediaTrack* requireTrack(int idx) {
    MediaTrack* t = GetTrack(kCur, idx);
    if (!t) throw std::runtime_error("track index out of range: " + std::to_string(idx));
    return t;
}
std::string playStateStr(int st) {
    if (st & 4) return "recording";
    if (st & 1) return "playing";
    if (st & 2) return "paused";
    return "stopped";
}
Json trackToJson(int idx, MediaTrack* t) {
    return Json{{"index", idx},
                {"name", trackName(t)},
                {"guid", trackGuid(t)},
                {"volDb", gainToDb(GetMediaTrackInfo_Value(t, "D_VOL"))},
                {"pan", GetMediaTrackInfo_Value(t, "D_PAN")},
                {"channels", (int)GetMediaTrackInfo_Value(t, "I_NCHAN")},
                {"muted", GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5},
                {"soloed", GetMediaTrackInfo_Value(t, "I_SOLO") > 0.5}};
}

// Read a track's full state chunk, growing the buffer until it fits (FX/envelope-heavy tracks can be
// large). Mirrors the read path behind the reaper://track/{index}/chunk resource.
std::string readTrackChunk(MediaTrack* t) {
    std::size_t cap = 1u << 20;  // 1 MiB
    for (int tries = 0; tries < 6; ++tries) {   // up to 32 MiB
        std::vector<char> buf(cap, '\0');
        if (GetTrackStateChunk(t, buf.data(), (int)buf.size(), false)) {
            std::string s(buf.data());
            if (s.size() + 1 < cap) return s;   // fit (not butting the buffer end -> not truncated)
        }
        cap <<= 1;
    }
    std::vector<char> buf(cap, '\0');
    GetTrackStateChunk(t, buf.data(), (int)buf.size(), false);
    return std::string(buf.data());
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace

void registerCoreTools(ToolRegistry& reg) {
    // ---- transport.get_state (read-only) ----
    reg.add(Tool{
        "transport.get_state",
        "Get transport state: play/pause/record, edit cursor, play position, tempo, loop.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "playState":{"type":"string","enum":["stopped","playing","paused","recording"]},
            "cursorSec":{"type":"number"},"playPositionSec":{"type":"number"},
            "tempo":{"type":"number"},"loop":{"type":"boolean"}},
            "required":["playState","cursorSec","tempo","loop"]})"),
        ToolAnnotations{/*readOnly*/ true, false, /*idempotent*/ true},
        Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const int st = GetPlayState();
            return Json{{"playState", playStateStr(st)},
                        {"cursorSec", GetCursorPosition()},
                        {"playPositionSec", GetPlayPosition()},
                        {"tempo", Master_GetTempo()},
                        {"loop", GetSetRepeat(-1) > 0}};
#else
            return Json{{"playState", "stopped"}, {"cursorSec", 0.0},
                        {"playPositionSec", 0.0}, {"tempo", 120.0}, {"loop", false}};
#endif
        }});

    // ---- transport.set (control) ----
    reg.add(Tool{
        "transport.set", "Control transport: play | stop | pause | record | goto(sec).",
        jparse(R"({"type":"object","properties":{"action":{"type":"string",
            "enum":["play","stop","pause","record","goto"]},"sec":{"type":"number"}},
            "required":["action"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"action":{"type":"string"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const std::string action = reqStr(a, "action");
#ifdef REAPER_MCP_HAVE_SDK
            if (action == "play") CSurf_OnPlay();
            else if (action == "stop") CSurf_OnStop();
            else if (action == "record") CSurf_OnRecord();
            else if (action == "pause") Main_OnCommand(1008, 0);  // Transport: Pause
            else if (action == "goto") SetEditCurPos(optNum(a, "sec", 0.0), true, false);
            else throw std::runtime_error("unknown action: " + action);
#endif
            return Json{{"ok", true}, {"action", action}};
        }});

    // ---- transport.set_repeat (loop on/off; idempotent) ----
    reg.add(Tool{
        "transport.set_repeat", "Turn the transport repeat/loop on or off.",
        jparse(R"({"type":"object","properties":{"enabled":{"type":"boolean"}},
            "required":["enabled"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"loop":{"type":"boolean"}},
            "required":["ok","loop"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true}, Profile::Core,
        [](const Json& a) -> Json {
            const bool en = reqBool(a, "enabled");
#ifdef REAPER_MCP_HAVE_SDK
            GetSetRepeat(en ? 1 : 0);
            return Json{{"ok", true}, {"loop", GetSetRepeat(-1) > 0}};
#else
            return Json{{"ok", true}, {"loop", en}};
#endif
        }});

    // ---- transport.set_playrate (master play rate; idempotent) ----
    reg.add(Tool{
        "transport.set_playrate", "Set the master play rate (0.25..4.0; 1.0 = normal speed).",
        jparse(R"({"type":"object","properties":{"rate":{"type":"number","minimum":0.25,"maximum":4}},
            "required":["rate"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"rate":{"type":"number"}},
            "required":["ok","rate"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            double rate = reqNum(a, "rate");
            if (rate < 0.25) rate = 0.25;
            if (rate > 4.0) rate = 4.0;
#ifdef REAPER_MCP_HAVE_SDK
            CSurf_OnPlayRateChange(rate);
            return Json{{"ok", true}, {"rate", Master_GetPlayRate(kCur)}};
#else
            return Json{{"ok", true}, {"rate", rate}};
#endif
        }});

    // ---- transport.set_metronome (click on/off; idempotent via read-after-write) ----
    reg.add(Tool{
        "transport.set_metronome",
        "Turn the metronome (click) on or off. Wraps REAPER action 40364 and reads its toggle state "
        "back so it lands on the requested state regardless of the starting state.",
        jparse(R"({"type":"object","properties":{"enabled":{"type":"boolean"}},
            "required":["enabled"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"enabled":{"type":"boolean"}},
            "required":["ok","enabled"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const bool want = reqBool(a, "enabled");
#ifdef REAPER_MCP_HAVE_SDK
            const int cur = GetToggleCommandState(40364);  // Options: Toggle metronome
            if ((cur > 0) != want) Main_OnCommand(40364, 0);
            const int now = GetToggleCommandState(40364);
            // now: 1 = on, 0 = off, -1 = no toggle state reported -> echo the requested state.
            return Json{{"ok", true}, {"enabled", now < 0 ? want : (now > 0)}};
#else
            return Json{{"ok", true}, {"enabled", want}};
#endif
        }});

    // ---- project.get_summary (read-only) ----
    reg.add(Tool{
        "project.get_summary", "Summarize the project: name, sample rate, track count, length.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"name":{"type":"string"},
            "sampleRate":{"type":"integer"},"trackCount":{"type":"integer"},
            "lengthSec":{"type":"number"}},"required":["name","trackCount","lengthSec"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            char nm[1024] = {0};
            GetProjectName(kCur, nm, sizeof(nm));
            std::string name = nm[0] ? nm : "Untitled";
            return Json{{"name", name},
                        {"sampleRate", (int)GetSetProjectInfo(kCur, "PROJECT_SRATE", 0, false)},
                        {"trackCount", CountTracks(kCur)},
                        {"lengthSec", GetProjectLength(kCur)}};
#else
            return Json{{"name", "Untitled"}, {"sampleRate", 48000},
                        {"trackCount", 0}, {"lengthSec", 0.0}};
#endif
        }});

    // ========================= project file + render =========================
    // The open/new verbs are modal hazards (a "save changes?" prompt would block the main-thread pump
    // that drains our request queue — the App Nap invariant). They are made pump-safe by refusing when
    // the project is dirty, and project.open uses Main_openProject's 'noprompt:' prefix when the caller
    // explicitly opts to discard. project.render uses the no-dialog render action (41824) and, by
    // default, only DRY-RUNs (a real render blocks the main thread for the render's duration).

    // ---- project.save (save to the existing path, or save-as to 'path'; no dialog) ----
    reg.add(Tool{
        "project.save", "Save the current project. With 'path', saves-as to that .rpp path (no dialog) and "
                        "makes it the project file. Without 'path', saves to the existing file; if the "
                        "project has never been saved, returns a 'no_project_path' error rather than "
                        "opening a blocking Save-As dialog.",
        jparse(R"({"type":"object","properties":{"path":{"type":"string"}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"path":{"type":"string"},
            "savedAs":{"type":"boolean"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true}, Profile::Core,
        [](const Json& a) -> Json {
            const bool hasPath = a.contains("path") && a["path"].is_string();
#ifdef REAPER_MCP_HAVE_SDK
            if (hasPath) {
                const std::string path = a["path"].get<std::string>();
                Main_SaveProjectEx(kCur, path.c_str(), 8);  // &8 = set as the new project filename
                return Json{{"ok", true}, {"path", path}, {"savedAs", true}};
            }
            char nm[1024] = {0};
            GetProjectName(kCur, nm, sizeof(nm));
            if (!nm[0])
                return makeError("no_project_path",
                                 "the project has never been saved, so save-to-existing would open a "
                                 "blocking Save-As dialog",
                                 "call project.save with a 'path' to save-as without a dialog");
            char dir[2048] = {0};
            GetProjectPath(dir, sizeof(dir));
            Main_SaveProject(kCur, false);
            return Json{{"ok", true}, {"path", std::string(dir) + "/" + nm}, {"savedAs", false}};
#else
            return Json{{"ok", true},
                        {"path", hasPath ? a["path"].get<std::string>() : "Untitled.rpp"},
                        {"savedAs", hasPath}};
#endif
        }});

    // ---- project.open (open a .rpp by path; pump-safe, dirty-guarded) ----
    reg.add(Tool{
        "project.open", "Open a project file by absolute path. Refuses with 'unsaved_changes' if the "
                        "current project is dirty, unless 'discardChanges':true (which opens via "
                        "REAPER's no-prompt path, discarding the current project's unsaved edits). Never "
                        "shows a blocking dialog.",
        jparse(R"({"type":"object","properties":{"path":{"type":"string"},
            "discardChanges":{"type":"boolean","default":false}},
            "required":["path"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"path":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}},
            "required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const std::string path = reqStr(a, "path");
            const bool discard = optBool(a, "discardChanges", false);
#ifdef REAPER_MCP_HAVE_SDK
            { std::ifstream f(path); if (!f.good())
                return makeError("file_not_found", "no readable project file at '" + path + "'",
                                 "pass an absolute path to an existing .rpp file"); }
            if (!discard && IsProjectDirty(kCur))
                return makeError("unsaved_changes",
                                 "the current project has unsaved changes; opening would discard them",
                                 "save with project.save first, or pass discardChanges:true");
            const std::string name = (discard ? std::string("noprompt:") : std::string()) + path;
            Main_openProject(name.c_str());
            return Json{{"ok", true}, {"path", path}};
#else
            (void)discard;
            return Json{{"ok", true}, {"path", path}};
#endif
        }});

    // ---- project.new (new empty project; pump-safe, dirty-guarded) ----
    reg.add(Tool{
        "project.new", "Start a new empty project. Refuses with 'unsaved_changes' if the current project "
                       "is dirty (a new-project action on a dirty project would open a blocking Save "
                       "prompt); save or open a project first. Never shows a blocking dialog.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},
            "detail":{"type":"string"},"remediation":{"type":"string"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            if (IsProjectDirty(kCur))
                return makeError("unsaved_changes",
                                 "the current project has unsaved changes; a new project would open a "
                                 "blocking Save prompt",
                                 "save with project.save, or open another project with "
                                 "project.open(discardChanges:true), then retry");
            Main_OnCommand(40023, 0);  // File: New project (no prompt on a clean project)
            return Json{{"ok", true}};
#else
            return Json{{"ok", true}};
#endif
        }});

    // ---- project.render (no-dialog render using the project's own render settings) ----
    reg.add(Tool{
        "project.render",
        "Render the project to disk using its already-configured render settings (format, channels, "
        "sources) — a plain, non-immersive render distinct from spatial.render_deliverables. Optionally "
        "override the output directory ('path'), file-name 'pattern', render bounds ('boundsFlag' + "
        "'startPos'/'endPos'), and 'addToProject'. dryRun (the DEFAULT) reports the exact output files "
        "the current settings would write WITHOUT rendering; set dryRun:false to actually render (this "
        "blocks the main thread for the render's duration — bound long projects with startPos/endPos). "
        "All overridden RENDER_* settings are snapshotted and restored.",
        jparse(R"({"type":"object","properties":{
            "path":{"type":"string"},"pattern":{"type":"string"},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "addToProject":{"type":"boolean"},
            "dryRun":{"type":"boolean","default":true}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"dryRun":{"type":"boolean"},
            "targets":{"type":"array","items":{"type":"string"}},"stats":{"type":"string"},
            "warnings":{"type":"array","items":{"type":"string"}}},"required":["ok","dryRun"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false}, Profile::Render,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", true);
            Json warnings = Json::array();
#ifdef REAPER_MCP_HAVE_SDK
            std::vector<char> buf(32768);
            auto getStr = [&](const char* k) -> std::string {
                buf.assign(32768, 0);
                GetSetProjectInfo_String(kCur, k, buf.data(), false);
                return std::string(buf.data());
            };
            auto setStr = [&](const char* k, const std::string& v) {
                std::vector<char> nb(v.begin(), v.end()); nb.push_back('\0');
                GetSetProjectInfo_String(kCur, k, nb.data(), true);
            };
            auto getNum = [&](const char* k) -> double { return GetSetProjectInfo(kCur, k, 0.0, false); };
            auto setNum = [&](const char* k, double v) { GetSetProjectInfo(kCur, k, v, true); };

            // Snapshot the fields we may override.
            const std::string sFile = getStr("RENDER_FILE"), sPat = getStr("RENDER_PATTERN");
            const double sBounds = getNum("RENDER_BOUNDSFLAG"), sStart = getNum("RENDER_STARTPOS"),
                         sEnd = getNum("RENDER_ENDPOS"), sAdd = getNum("RENDER_ADDTOPROJ");

            auto restore = [&]() {
                setStr("RENDER_FILE", sFile); setStr("RENDER_PATTERN", sPat);
                setNum("RENDER_BOUNDSFLAG", sBounds); setNum("RENDER_STARTPOS", sStart);
                setNum("RENDER_ENDPOS", sEnd); setNum("RENDER_ADDTOPROJ", sAdd);
            };

            // Apply overrides.
            if (a.contains("path") && a["path"].is_string())       setStr("RENDER_FILE", a["path"].get<std::string>());
            if (a.contains("pattern") && a["pattern"].is_string()) setStr("RENDER_PATTERN", a["pattern"].get<std::string>());
            if (a.contains("boundsFlag") && a["boundsFlag"].is_number()) setNum("RENDER_BOUNDSFLAG", (double)a["boundsFlag"].get<int>());
            if (a.contains("startPos") && a["startPos"].is_number())  setNum("RENDER_STARTPOS", a["startPos"].get<double>());
            if (a.contains("endPos") && a["endPos"].is_number())      setNum("RENDER_ENDPOS", a["endPos"].get<double>());
            if (a.contains("addToProject") && a["addToProject"].is_boolean()) setNum("RENDER_ADDTOPROJ", a["addToProject"].get<bool>() ? 1.0 : 0.0);

            // The output file list the current settings resolve to.
            Json targets = Json::array();
            {
                const std::string tstr = getStr("RENDER_TARGETS");
                std::string cur;
                for (char c : tstr) { if (c == ';') { if (!cur.empty()) targets.push_back(cur); cur.clear(); } else cur += c; }
                if (!cur.empty()) targets.push_back(cur);
            }
            if (targets.empty())
                warnings.push_back("RENDER_TARGETS is empty — RENDER_FILE/RENDER_PATTERN may be unset; the "
                                   "render would have no output path.");

            if (dryRun) {
                restore();
                Json out{{"ok", true}, {"dryRun", true}, {"targets", targets}};
                if (!warnings.empty()) out["warnings"] = warnings;
                return out;
            }
            Main_OnCommand(41824, 0);  // File: Render project to disk, most-recent settings — no dialog
            const std::string stats = getStr("RENDER_STATS");
            restore();
            Json out{{"ok", true}, {"dryRun", false}, {"targets", targets}, {"stats", stats}};
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
#else
            (void)a;
            Json out{{"ok", true}, {"dryRun", dryRun}, {"targets", Json::array()}};
            return out;
#endif
        }});

    // ---- track.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "track.add", "Insert a new track at an index (default: end) and optionally name it.",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer","minimum":0},
            "name":{"type":"string"}},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"trackIndex":{"type":"integer"},
            "guid":{"type":"string"}},"required":["trackIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            int idx = a.contains("index") && a["index"].is_number() ? a["index"].get<int>()
                                                                    : CountTracks(kCur);
            Undo_BeginBlock2(kCur);
            InsertTrackAtIndex(idx, true);
            TrackList_AdjustWindows(false);
            MediaTrack* t = GetTrack(kCur, idx);
            if (a.contains("name") && a["name"].is_string() && t) {
                std::string nm = a["name"].get<std::string>();
                std::vector<char> nb(nm.begin(), nm.end());
                nb.push_back('\0');
                GetSetMediaTrackInfo_String(t, "P_NAME", nb.data(), true);
            }
            Undo_EndBlock2(kCur, "MCP: add track", -1);
            return Json{{"trackIndex", idx}, {"guid", trackGuid(t)}};
#else
            int idx = a.contains("index") && a["index"].is_number() ? a["index"].get<int>() : 0;
            return Json{{"trackIndex", idx},
                        {"guid", "{00000000-0000-0000-0000-000000000000}"}};
#endif
        }});

    // ---- track.set_fader (mutating) ----
    reg.add(Tool{
        "track.set_fader", "Set a track's volume (dB) and/or pan (-1..1).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "db":{"type":"number"},"pan":{"type":"number","minimum":-1,"maximum":1}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"db":{"type":"number"},
            "pan":{"type":"number"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (a.contains("db") && a["db"].is_number())
                SetMediaTrackInfo_Value(t, "D_VOL", dbToGain(a["db"].get<double>()));
            if (a.contains("pan") && a["pan"].is_number()) {
                double p = a["pan"].get<double>();
                p = p < -1.0 ? -1.0 : (p > 1.0 ? 1.0 : p);
                SetMediaTrackInfo_Value(t, "D_PAN", p);
            }
            Undo_EndBlock2(kCur, "MCP: set track fader", -1);
            return Json{{"ok", true},
                        {"db", gainToDb(GetMediaTrackInfo_Value(t, "D_VOL"))},
                        {"pan", GetMediaTrackInfo_Value(t, "D_PAN")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"db", optNum(a, "db", 0.0)}, {"pan", optNum(a, "pan", 0.0)}};
#endif
        }});

    // ---- track.set_state_chunk (elicitation-gated wholesale state replace) ----
    // Replaces a track's ENTIRE state (FX chain, routing, envelopes, items, layout) from a raw REAPER
    // <TRACK ...> chunk — the write-side complement of the read resource reaper://track/{index}/chunk.
    // Because it wholesale-replaces state, it is gated behind the elicitation lane: an
    // elicitation-capable client confirms via a real elicitation/create round-trip (the handler THROWS
    // before any edit); clients without elicitation pass confirm:true. Rails: (a) validate the chunk
    // (<TRACK.. + balanced blocks) before anything; (b) dryRun returns a unified diff of
    // current-vs-proposed and mutates nothing; (c) apply in one undo block; (d) an unchanged proposal
    // is a no-op regardless of confirm.
    reg.add(Tool{
        "track.set_state_chunk",
        "Replace a track's entire state from a raw REAPER <TRACK ...> chunk (FX, routing, envelopes, "
        "items — the write complement of reaper://track/{index}/chunk). DESTRUCTIVE wholesale replace: "
        "an elicitation-capable client confirms via an MCP elicitation round-trip; others pass "
        "confirm:true. dryRun (preview) returns a unified diff of current-vs-proposed and changes "
        "nothing. The chunk is structurally validated first; an unchanged proposal is a no-op.",
        jparse(R"({"type":"object","properties":{
            "track":{"type":"integer","minimum":0},
            "chunk":{"type":"string"},
            "confirm":{"type":"boolean","default":false},
            "dryRun":{"type":"boolean","default":false},
            "context":{"type":"integer","minimum":0,"default":3}},
            "required":["track","chunk"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "applied":{"type":"boolean"},"changed":{"type":"boolean"},"dryRun":{"type":"boolean"},
            "diff":{"type":"string"},"addedLines":{"type":"integer"},"removedLines":{"type":"integer"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "note":{"type":"string"}},"required":["ok","track"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string chunk = reqStr(a, "chunk");
            const bool confirm = optBool(a, "confirm", false);
            const bool dryRun = optBool(a, "dryRun", false);
            const int context = optInt(a, "context", 3);
            const std::size_t ctx = (std::size_t)(context < 0 ? 0 : context);

            // Rail (a): structural validation before touching anything (SDK-free).
            const std::string vErr = validateTrackChunk(chunk);
            if (!vErr.empty())
                return makeError("invalid_chunk", vErr,
                                 "pass a complete <TRACK ...> state chunk, e.g. a lightly-edited copy "
                                 "of reaper://track/{index}/chunk");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const std::string current = readTrackChunk(t);
            const ChunkDiff d = diffTrackChunks(current, chunk, ctx);

            // A proposal identical to current state is a no-op — never elicit or write for nothing.
            if (!d.changed)
                return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", false},
                            {"dryRun", dryRun}, {"diff", ""}, {"addedLines", 0}, {"removedLines", 0},
                            {"note", "proposed chunk matches the track's current state; nothing to apply"}};

            // Rail (b): dryRun previews the unified diff and mutates nothing.
            if (dryRun)
                return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", true},
                            {"dryRun", true}, {"diff", d.text}, {"addedLines", d.added},
                            {"removedLines", d.removed}};

            // Rail (d): confirmation gate. requireElicitation() THROWS for an elicitation-capable client
            // (transport prompts the human, re-invokes with confirm:true); otherwise it no-ops and we
            // return the confirmation_required fallback carrying the diff the caller is asked to OK.
            if (!confirm) {
                requireElicitation("track.set_state_chunk will REPLACE the entire state of track " +
                                   std::to_string(idx) + " ('" + trackName(t) + "') — its FX chain, "
                                   "routing, envelopes, and items (+" + std::to_string(d.added) + "/-" +
                                   std::to_string(d.removed) + " lines). Proceed?");
                return Json{{"ok", false}, {"error", "confirmation_required"}, {"track", idx},
                            {"applied", false}, {"changed", true}, {"dryRun", false}, {"diff", d.text},
                            {"addedLines", d.added}, {"removedLines", d.removed},
                            {"detail", "wholesale replace of track " + std::to_string(idx) + " state (+" +
                                           std::to_string(d.added) + "/-" + std::to_string(d.removed) +
                                           " lines)"},
                            {"remediation", "re-run with confirm:true (or dryRun:true to preview), or "
                                            "accept the elicitation prompt"}};
            }

            // Rail (c): apply in one undo block; roll back if REAPER rejects the chunk.
            UndoTransaction txn("MCP: set track " + std::to_string(idx) + " state chunk");
            if (!SetTrackStateChunk(t, chunk.c_str(), false))
                throw CompositeError("apply_failed",
                                     "REAPER rejected the state chunk for track " + std::to_string(idx),
                                     "verify the chunk is a valid <TRACK ...> block (start from "
                                     "reaper://track/{index}/chunk and edit conservatively)");
            txn.recordExecuted("set_state_chunk", "track " + std::to_string(idx),
                               "+" + std::to_string(d.added) + "/-" + std::to_string(d.removed) + " lines");
            txn.commit();
            return Json{{"ok", true}, {"track", idx}, {"applied", true}, {"changed", true},
                        {"dryRun", false}, {"diff", d.text}, {"addedLines", d.added},
                        {"removedLines", d.removed}};
#else
            // Host build (no REAPER): prove validation + the diff/preview envelope against an empty
            // baseline (all-insert). The real read/apply + elicitation round-trip run under REAPER.
            (void)confirm;  // gate lives in the SDK branch; host path is preview-only
            const ChunkDiff d = diffTrackChunks(std::string(), chunk, ctx);
            return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", d.changed},
                        {"dryRun", dryRun}, {"diff", d.text}, {"addedLines", d.added},
                        {"removedLines", d.removed}, {"note", "host build (no REAPER): preview only"}};
#endif
        }});

    // ---- track.patch_state (targeted top-level chunk-key patch on the state-chunk write path) ----
    // A narrower, safer sibling of track.set_state_chunk: instead of replacing a track's WHOLE state,
    // it rewrites only the value of named TOP-LEVEL keys of the <TRACK ...> block (NAME, PEAKCOL,
    // VOLPAN, MUTESOLO, REC …), leaving every nested block (FX chain, items, envelopes) untouched. It
    // reuses the SAME proven write rails: read the current chunk, apply the targeted key replacements
    // (SDK-free), structurally validate, (b) dryRun returns a unified diff and mutates nothing, gate
    // (d) elicitation/confirm before writing, apply (c) in one undo block. Because it always patches
    // the FRESHLY-READ current chunk, it is immune to REAPER's chunk re-serialization:
    // patching a key to its current value is a genuine no-op. A key that is not present at the top
    // level fails closed (key_not_found) rather than being invented.
    reg.add(Tool{
        "track.patch_state",
        "Patch specific TOP-LEVEL keys of a track's state chunk in place (e.g. NAME, PEAKCOL, VOLPAN, "
        "MUTESOLO, REC) — a narrower, safer alternative to track.set_state_chunk's wholesale replace. "
        "Reads the current <TRACK ...> chunk, rewrites each named key's line, and applies via the same "
        "validated write path (structural validation, dry-run unified diff, single undo block, "
        "elicitation/confirm gate). Only EXISTING top-level keys are patched; a key inside a nested "
        "block (FX chain, items, envelopes) is not reachable here (use track.set_state_chunk). An "
        "unchanged patch is a no-op.",
        jparse(R"({"type":"object","properties":{
            "track":{"type":"integer","minimum":0},
            "patches":{"type":"array","minItems":1,"items":{"type":"object","properties":{
                "key":{"type":"string"},"value":{"type":"string"}},
                "required":["key","value"],"additionalProperties":false}},
            "confirm":{"type":"boolean","default":false},
            "dryRun":{"type":"boolean","default":false},
            "context":{"type":"integer","minimum":0,"default":3}},
            "required":["track","patches"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "applied":{"type":"boolean"},"changed":{"type":"boolean"},"dryRun":{"type":"boolean"},
            "keys":{"type":"array","items":{"type":"string"}},"diff":{"type":"string"},
            "addedLines":{"type":"integer"},"removedLines":{"type":"integer"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "note":{"type":"string"}},"required":["ok","track"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            if (!a.contains("patches") || !a["patches"].is_array() || a["patches"].empty())
                throw std::runtime_error("missing/invalid array arg: patches");
            std::vector<ChunkKeyPatch> patches;
            for (const auto& p : a["patches"]) {
                if (!p.is_object() || !p.contains("key") || !p["key"].is_string() ||
                    !p.contains("value") || !p["value"].is_string())
                    throw std::runtime_error("each patch needs a string 'key' and a string 'value'");
                patches.push_back({p["key"].get<std::string>(), p["value"].get<std::string>()});
            }
            const bool confirm = optBool(a, "confirm", false);
            const bool dryRun = optBool(a, "dryRun", false);
            const int context = optInt(a, "context", 3);
            const std::size_t ctx = (std::size_t)(context < 0 ? 0 : context);

            // Shape-validate the patch set before touching anything (SDK-free).
            const std::string pErr = validateKeyPatches(patches);
            if (!pErr.empty())
                return makeError("invalid_patch", pErr,
                                 "each patch key is a single top-level chunk bareword (e.g. NAME, "
                                 "PEAKCOL); values must be single-line; keys must be unique");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const std::string current = readTrackChunk(t);
            ChunkPatchResult pr = applyTrackChunkKeyPatches(current, patches);
            if (!pr.error.empty()) {
                if (pr.error == "key_not_found")
                    return makeError("key_not_found", pr.detail,
                                     "only EXISTING top-level track keys can be patched; read "
                                     "reaper://track/" + std::to_string(idx) + "/chunk to see them, or "
                                     "use track.set_state_chunk for nested/structural edits");
                return makeError(pr.error, pr.detail, "");
            }
            const std::string& proposed = pr.chunk;

            // Rail (a): the patched chunk must still be structurally valid (defensive — a scalar-value
            // rewrite should always keep it balanced).
            const std::string vErr = validateTrackChunk(proposed);
            if (!vErr.empty())
                return makeError("invalid_chunk", vErr,
                                 "the patched chunk failed structural validation; check the values");

            Json keys = Json::array();
            for (const std::string& kk : pr.appliedKeys) keys.push_back(kk);
            const ChunkDiff d = diffTrackChunks(current, proposed, ctx);

            // An effectively-unchanged patch is a no-op — never elicit or write for nothing.
            if (!d.changed)
                return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", false},
                            {"dryRun", dryRun}, {"keys", keys}, {"diff", ""}, {"addedLines", 0},
                            {"removedLines", 0},
                            {"note", "patched values match the track's current state; nothing to apply"}};

            // Rail (b): dryRun previews the unified diff and mutates nothing.
            if (dryRun)
                return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", true},
                            {"dryRun", true}, {"keys", keys}, {"diff", d.text},
                            {"addedLines", d.added}, {"removedLines", d.removed}};

            // Rail (d): confirmation gate. requireElicitation() THROWS for an elicitation-capable
            // client (transport prompts the human, re-invokes with confirm:true); otherwise it no-ops
            // and we return the confirmation_required fallback carrying the diff.
            if (!confirm) {
                requireElicitation("track.patch_state will change " +
                                   std::to_string(pr.appliedKeys.size()) + " top-level key(s) on track " +
                                   std::to_string(idx) + " ('" + trackName(t) + "') (+" +
                                   std::to_string(d.added) + "/-" + std::to_string(d.removed) +
                                   " lines). Proceed?");
                return Json{{"ok", false}, {"error", "confirmation_required"}, {"track", idx},
                            {"applied", false}, {"changed", true}, {"dryRun", false}, {"keys", keys},
                            {"diff", d.text}, {"addedLines", d.added}, {"removedLines", d.removed},
                            {"detail", "patch " + std::to_string(pr.appliedKeys.size()) +
                                           " top-level key(s) on track " + std::to_string(idx) + " (+" +
                                           std::to_string(d.added) + "/-" + std::to_string(d.removed) +
                                           " lines)"},
                            {"remediation", "re-run with confirm:true (or dryRun:true to preview), or "
                                            "accept the elicitation prompt"}};
            }

            // Rail (c): apply in one undo block; roll back if REAPER rejects the chunk.
            UndoTransaction txn("MCP: patch track " + std::to_string(idx) + " state");
            if (!SetTrackStateChunk(t, proposed.c_str(), false))
                throw CompositeError("apply_failed",
                                     "REAPER rejected the patched state chunk for track " +
                                         std::to_string(idx),
                                     "a patched value may be malformed for its key; verify against "
                                     "reaper://track/{index}/chunk");
            txn.recordExecuted("patch_state", "track " + std::to_string(idx),
                               "+" + std::to_string(d.added) + "/-" + std::to_string(d.removed) + " lines");
            txn.commit();
            return Json{{"ok", true}, {"track", idx}, {"applied", true}, {"changed", true},
                        {"dryRun", false}, {"keys", keys}, {"diff", d.text}, {"addedLines", d.added},
                        {"removedLines", d.removed}};
#else
            // Host build (no REAPER): exercise the SDK-free patch helper against a synthetic baseline so
            // key resolution + the diff/preview envelope are unit-provable with no running REAPER. The
            // real read/apply + elicitation round-trip run under REAPER.
            (void)confirm;
            const std::string synthetic =
                "<TRACK {GUID}\nNAME \"Baseline\"\nPEAKCOL 16576\nVOLPAN 1 0 -1 -1 1\n"
                "<FXCHAIN\nBYPASS 0 0 0\n>\n>\n";
            ChunkPatchResult pr = applyTrackChunkKeyPatches(synthetic, patches);
            if (!pr.error.empty()) {
                if (pr.error == "key_not_found")
                    return makeError("key_not_found", pr.detail,
                                     "host build: only NAME/PEAKCOL/VOLPAN exist in the synthetic baseline");
                return makeError(pr.error, pr.detail, "");
            }
            Json keys = Json::array();
            for (const std::string& kk : pr.appliedKeys) keys.push_back(kk);
            const ChunkDiff d = diffTrackChunks(synthetic, pr.chunk, ctx);
            return Json{{"ok", true}, {"track", idx}, {"applied", false}, {"changed", d.changed},
                        {"dryRun", dryRun}, {"keys", keys}, {"diff", d.text}, {"addedLines", d.added},
                        {"removedLines", d.removed}, {"note", "host build (no REAPER): preview only"}};
#endif
        }});

    // ---- track.list (read-only) ----
    reg.add(Tool{
        "track.list", "List all tracks with index, name, GUID, volume (dB), pan, channels, mute/solo.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"trackCount":{"type":"integer"},
            "tracks":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"name":{"type":"string"},"guid":{"type":"string"},
                "volDb":{"type":"number"},"pan":{"type":"number"},"channels":{"type":"integer"},
                "muted":{"type":"boolean"},"soloed":{"type":"boolean"}}}}},
            "required":["trackCount","tracks"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const int n = CountTracks(kCur);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) arr.push_back(trackToJson(i, GetTrack(kCur, i)));
            return Json{{"trackCount", n}, {"tracks", std::move(arr)}};
#else
            return Json{{"trackCount", 0}, {"tracks", Json::array()}};
#endif
        }});

    // ---- track.select (mutating; not an undo point) ----
    reg.add(Tool{
        "track.select", "Select a track; exclusive=true (default) clears other selections.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "exclusive":{"type":"boolean","default":true}},"required":["track"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool exclusive = optBool(a, "exclusive", true);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            if (exclusive) SetOnlyTrackSelected(t);
            else SetTrackSelected(t, true);
#else
            (void)idx; (void)exclusive;
#endif
            return Json{{"ok", true}};
        }});

    // ---- track.remove (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "track.remove", "Delete a track by index. Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "removedIndex":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            DeleteTrack(t);
            TrackList_AdjustWindows(false);
            Undo_EndBlock2(kCur, "MCP: remove track", -1);
#else
            (void)idx;
#endif
            return Json{{"ok", true}, {"removedIndex", idx}};
        }});

    // ---- track.get_name (read-only) ----
    reg.add(Tool{
        "track.get_name", "Get a track's name by index.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            return Json{{"name", trackName(requireTrack(idx))}};
#else
            return Json{{"name", "Track " + std::to_string(idx)}};
#endif
        }});

    // ---- track.set_name (mutating) ----
    reg.add(Tool{
        "track.set_name", "Rename a track by index.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "name":{"type":"string"}},"required":["track","name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"name":{"type":"string"}},
            "required":["ok","name"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "name");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            std::vector<char> nb(name.begin(), name.end());
            nb.push_back('\0');
            GetSetMediaTrackInfo_String(t, "P_NAME", nb.data(), true);
            Undo_EndBlock2(kCur, "MCP: rename track", -1);
#else
            (void)idx;
#endif
            return Json{{"ok", true}, {"name", name}};
        }});

    // =====================================================================================
    // Granular per-field track setters — each is a
    // distinct tool with a REQUIRED value arg and is idempotent; consolidated reads live in
    // track.get / track.list. Volume-dB + pan stay in track.set_fader (established convention).
    // Every field maps 1:1 to SetMediaTrackInfo_Value and is wrapped in one undo block; atomic
    // multi-field edits are composed via session.run_dsl (single undo), not a fat setter.
    // =====================================================================================

    // ---- track.set_mute (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_mute", "Mute or unmute a track (B_MUTE).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "muted":{"type":"boolean"}},"required":["track","muted"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"muted":{"type":"boolean"}},
            "required":["ok","muted"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool v = reqBool(a, "muted");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "B_MUTE", v ? 1.0 : 0.0);
            Undo_EndBlock2(kCur, "MCP: set track mute", -1);
            return Json{{"ok", true}, {"muted", GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5}};
#else
            (void)idx;
            return Json{{"ok", true}, {"muted", v}};
#endif
        }});

    // ---- track.set_solo (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_solo", "Solo or unsolo a track (I_SOLO; solo = solo-in-place).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "soloed":{"type":"boolean"}},"required":["track","soloed"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"soloed":{"type":"boolean"}},
            "required":["ok","soloed"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool v = reqBool(a, "soloed");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_SOLO", v ? 2.0 : 0.0);  // 2 = solo-in-place (REAPER default)
            Undo_EndBlock2(kCur, "MCP: set track solo", -1);
            return Json{{"ok", true}, {"soloed", GetMediaTrackInfo_Value(t, "I_SOLO") > 0.5}};
#else
            (void)idx;
            return Json{{"ok", true}, {"soloed", v}};
#endif
        }});

    // ---- track.set_phase (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_phase", "Invert (flip) a track's polarity/phase, or clear it (B_PHASE).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "inverted":{"type":"boolean"}},"required":["track","inverted"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"inverted":{"type":"boolean"}},
            "required":["ok","inverted"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool v = reqBool(a, "inverted");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "B_PHASE", v ? 1.0 : 0.0);
            Undo_EndBlock2(kCur, "MCP: set track phase", -1);
            return Json{{"ok", true}, {"inverted", GetMediaTrackInfo_Value(t, "B_PHASE") > 0.5}};
#else
            (void)idx;
            return Json{{"ok", true}, {"inverted", v}};
#endif
        }});

    // ---- track.set_width (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_width", "Set a track's stereo/pan width (D_WIDTH, -1..1; 1 = full width).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "width":{"type":"number","minimum":-1,"maximum":1}},
            "required":["track","width"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"width":{"type":"number"}},
            "required":["ok","width"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            double w = reqNum(a, "width");
            w = w < -1.0 ? -1.0 : (w > 1.0 ? 1.0 : w);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "D_WIDTH", w);
            Undo_EndBlock2(kCur, "MCP: set track width", -1);
            return Json{{"ok", true}, {"width", GetMediaTrackInfo_Value(t, "D_WIDTH")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"width", w}};
#endif
        }});

    // ---- track.set_color (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_color", "Set a track's custom color (0xRRGGBB); color:0 clears the custom color.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "color":{"type":"integer","minimum":0,"description":"0xRRGGBB; 0 = clear"}},
            "required":["track","color"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"color":{"type":"integer"},
            "colorSet":{"type":"boolean"}},"required":["ok","color","colorSet"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int rgb = reqInt(a, "color") & 0xFFFFFF;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            // REAPER stores custom colors with the 0x1000000 "is-set" flag; 0 = default/none.
            SetMediaTrackInfo_Value(t, "I_CUSTOMCOLOR", rgb ? (double)(rgb | 0x1000000) : 0.0);
            Undo_EndBlock2(kCur, "MCP: set track color", -1);
            const int c = (int)GetMediaTrackInfo_Value(t, "I_CUSTOMCOLOR");
            return Json{{"ok", true}, {"color", c & 0xFFFFFF}, {"colorSet", (c & 0x1000000) != 0}};
#else
            (void)idx;
            return Json{{"ok", true}, {"color", rgb}, {"colorSet", rgb != 0}};
#endif
        }});

    // ---- track.set_rec_arm (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_rec_arm", "Arm or disarm a track for recording (I_RECARM).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "armed":{"type":"boolean"}},"required":["track","armed"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"armed":{"type":"boolean"}},
            "required":["ok","armed"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool v = reqBool(a, "armed");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_RECARM", v ? 1.0 : 0.0);
            Undo_EndBlock2(kCur, "MCP: set track record-arm", -1);
            return Json{{"ok", true}, {"armed", GetMediaTrackInfo_Value(t, "I_RECARM") > 0.5}};
#else
            (void)idx;
            return Json{{"ok", true}, {"armed", v}};
#endif
        }});

    // ---- track.set_rec_input (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_rec_input", "Set a track's record input index (I_RECINPUT; mono in n, stereo, MIDI, "
                               "etc. — see the REAPER record-input encoding).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "input":{"type":"integer"}},"required":["track","input"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"input":{"type":"integer"}},
            "required":["ok","input"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int v = reqInt(a, "input");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_RECINPUT", (double)v);
            Undo_EndBlock2(kCur, "MCP: set track record-input", -1);
            return Json{{"ok", true}, {"input", (int)GetMediaTrackInfo_Value(t, "I_RECINPUT")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"input", v}};
#endif
        }});

    // ---- track.set_rec_mon (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_rec_mon", "Set a track's record monitoring mode (I_RECMON: 0 off, 1 on, 2 auto).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "mode":{"type":"integer","minimum":0,"maximum":2}},
            "required":["track","mode"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"mode":{"type":"integer"}},
            "required":["ok","mode"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            int m = reqInt(a, "mode");
            if (m < 0) m = 0;
            if (m > 2) m = 2;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_RECMON", (double)m);
            Undo_EndBlock2(kCur, "MCP: set track record-monitor", -1);
            return Json{{"ok", true}, {"mode", (int)GetMediaTrackInfo_Value(t, "I_RECMON")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"mode", m}};
#endif
        }});

    // ---- track.set_rec_mode (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_rec_mode",
        "Set a track's record mode (I_RECMODE: 0 input, 1 stereo out, 2 none/monitor-only, 3 stereo "
        "out w/latency comp, 4 midi output, 5 mono out, 6 mono out w/latency comp, 7 midi overdub, "
        "8 midi replace).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "mode":{"type":"integer","minimum":0,"maximum":8}},
            "required":["track","mode"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"mode":{"type":"integer"}},
            "required":["ok","mode"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            int m = reqInt(a, "mode");
            if (m < 0) m = 0;
            if (m > 8) m = 8;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_RECMODE", (double)m);
            Undo_EndBlock2(kCur, "MCP: set track record mode", -1);
            return Json{{"ok", true}, {"mode", (int)GetMediaTrackInfo_Value(t, "I_RECMODE")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"mode", m}};
#endif
        }});

    // ---- track.set_folder (mutating; idempotent) ----
    reg.add(Tool{
        "track.set_folder", "Set a track's folder depth (I_FOLDERDEPTH: 1 = folder parent, 0 = normal, "
                            "negative = close N folders after this track) and optional folder-compact "
                            "state (I_FOLDERCOMPACT: 0 open, 1 small, 2 closed).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "depth":{"type":"integer"},"compact":{"type":"integer","minimum":0,"maximum":2}},
            "required":["track","depth"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"depth":{"type":"integer"},
            "compact":{"type":"integer"}},"required":["ok","depth"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int depth = reqInt(a, "depth");
            const bool hasCompact = a.contains("compact") && a["compact"].is_number();
            int compact = optInt(a, "compact", 0);
            if (compact < 0) compact = 0;
            if (compact > 2) compact = 2;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_FOLDERDEPTH", (double)depth);
            if (hasCompact) SetMediaTrackInfo_Value(t, "I_FOLDERCOMPACT", (double)compact);
            TrackList_AdjustWindows(false);
            Undo_EndBlock2(kCur, "MCP: set track folder", -1);
            Json out{{"ok", true}, {"depth", (int)GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH")}};
            out["compact"] = (int)GetMediaTrackInfo_Value(t, "I_FOLDERCOMPACT");
            return out;
#else
            (void)idx;
            Json out{{"ok", true}, {"depth", depth}};
            if (hasCompact) out["compact"] = compact;
            return out;
#endif
        }});

    // ---- track.get (read-only, consolidated: every field the granular setters write) ----
    reg.add(Tool{
        "track.get", "Get one track's full state: name, GUID, volume (dB), pan, width, channels, "
                     "mute/solo/phase, record arm/input/monitor/mode, automation mode, folder "
                     "depth/compact, color.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},
            "guid":{"type":"string"},"volDb":{"type":"number"},"pan":{"type":"number"},
            "width":{"type":"number"},"channels":{"type":"integer"},"muted":{"type":"boolean"},
            "soloed":{"type":"boolean"},"phaseInverted":{"type":"boolean"},"recArmed":{"type":"boolean"},
            "recInput":{"type":"integer"},"recMon":{"type":"integer"},"recMode":{"type":"integer"},
            "automationMode":{"type":"integer"},"folderDepth":{"type":"integer"},
            "folderCompact":{"type":"integer"},"color":{"type":"integer"},"colorSet":{"type":"boolean"}},
            "required":["index","name","volDb","pan","channels","muted","soloed"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int col = (int)GetMediaTrackInfo_Value(t, "I_CUSTOMCOLOR");
            return Json{{"index", idx}, {"name", trackName(t)}, {"guid", trackGuid(t)},
                        {"volDb", gainToDb(GetMediaTrackInfo_Value(t, "D_VOL"))},
                        {"pan", GetMediaTrackInfo_Value(t, "D_PAN")},
                        {"width", GetMediaTrackInfo_Value(t, "D_WIDTH")},
                        {"channels", (int)GetMediaTrackInfo_Value(t, "I_NCHAN")},
                        {"muted", GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5},
                        {"soloed", GetMediaTrackInfo_Value(t, "I_SOLO") > 0.5},
                        {"phaseInverted", GetMediaTrackInfo_Value(t, "B_PHASE") > 0.5},
                        {"recArmed", GetMediaTrackInfo_Value(t, "I_RECARM") > 0.5},
                        {"recInput", (int)GetMediaTrackInfo_Value(t, "I_RECINPUT")},
                        {"recMon", (int)GetMediaTrackInfo_Value(t, "I_RECMON")},
                        {"recMode", (int)GetMediaTrackInfo_Value(t, "I_RECMODE")},
                        {"automationMode", GetTrackAutomationMode(t)},
                        {"folderDepth", (int)GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH")},
                        {"folderCompact", (int)GetMediaTrackInfo_Value(t, "I_FOLDERCOMPACT")},
                        {"color", col & 0xFFFFFF}, {"colorSet", (col & 0x1000000) != 0}};
#else
            return Json{{"index", idx}, {"name", "Track " + std::to_string(idx)},
                        {"guid", "{00000000-0000-0000-0000-000000000000}"}, {"volDb", 0.0},
                        {"pan", 0.0}, {"width", 1.0}, {"channels", 2}, {"muted", false},
                        {"soloed", false}, {"phaseInverted", false}, {"recArmed", false},
                        {"recInput", 0}, {"recMon", 0}, {"recMode", 0}, {"automationMode", 0},
                        {"folderDepth", 0}, {"folderCompact", 0},
                        {"color", 0}, {"colorSet", false}};
#endif
        }});

    // =====================================================================================
    // Freeze / unfreeze / peak metering. Freeze drives REAPER's own actions
    // (40901 mono / 41223 stereo / 40877 multichannel / 41644 unfreeze — selection-scoped, so
    // the tool isolates selection to the target track and restores it after). A freeze renders
    // synchronously: the call blocks until the render completes (keep material short for gates).
    // =====================================================================================

    // ---- track.freeze (mutating; DESTRUCTIVE — replaces items+FX with rendered audio) ----
    reg.add(Tool{
        "track.freeze",
        "Freeze a track (render items+FX to audio, take FX offline): mode 'stereo' (default), "
        "'mono', or 'multichannel' (use multichannel to preserve bed/ambisonic widths). Blocks "
        "until the render completes; reversible with track.unfreeze.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "mode":{"type":"string","enum":["stereo","mono","multichannel"],"default":"stereo"}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"mode":{"type":"string"},
            "fxCount":{"type":"integer"},"itemCount":{"type":"integer"}},"required":["ok","mode"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string mode = optStr(a, "mode", "stereo");
            int cmd = 41223;                            // Track: freeze to stereo
            if (mode == "mono") cmd = 40901;            // Track: freeze to mono
            else if (mode == "multichannel") cmd = 40877;  // Track: freeze to multichannel
            else if (mode != "stereo") throw std::runtime_error("mode must be stereo|mono|multichannel");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            // Isolate selection to the target (the action is selection-scoped), then restore.
            std::vector<MediaTrack*> prevSel;
            const int selN = CountSelectedTracks(kCur);
            prevSel.reserve((size_t)(selN > 0 ? selN : 0));
            for (int i = 0; i < selN; ++i) prevSel.push_back(GetSelectedTrack(kCur, i));
            SetOnlyTrackSelected(t);
            Undo_BeginBlock2(kCur);
            Main_OnCommand(cmd, 0);
            Undo_EndBlock2(kCur, "MCP: freeze track", -1);
            for (int i = 0, n = CountTracks(kCur); i < n; ++i)
                SetTrackSelected(GetTrack(kCur, i), false);
            for (MediaTrack* p : prevSel)
                if (p && ValidatePtr2(kCur, p, "MediaTrack*")) SetTrackSelected(p, true);
            UpdateArrange();
            return Json{{"ok", true}, {"mode", mode},
                        {"fxCount", TrackFX_GetCount(t)},
                        {"itemCount", CountTrackMediaItems(t)}};
#else
            (void)idx; (void)cmd;
            return Json{{"ok", true}, {"mode", mode}, {"fxCount", 0}, {"itemCount", 0}};
#endif
        }});

    // ---- track.unfreeze (mutating; restores pre-freeze items + FX) ----
    reg.add(Tool{
        "track.unfreeze",
        "Unfreeze a track (restore the items and FX saved by track.freeze). No-op if the track is "
        "not frozen.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxCount":{"type":"integer"},
            "itemCount":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            std::vector<MediaTrack*> prevSel;
            const int selN = CountSelectedTracks(kCur);
            prevSel.reserve((size_t)(selN > 0 ? selN : 0));
            for (int i = 0; i < selN; ++i) prevSel.push_back(GetSelectedTrack(kCur, i));
            SetOnlyTrackSelected(t);
            Undo_BeginBlock2(kCur);
            Main_OnCommand(41644, 0);  // Track: unfreeze tracks (restore previously saved items/FX)
            Undo_EndBlock2(kCur, "MCP: unfreeze track", -1);
            for (int i = 0, n = CountTracks(kCur); i < n; ++i)
                SetTrackSelected(GetTrack(kCur, i), false);
            for (MediaTrack* p : prevSel)
                if (p && ValidatePtr2(kCur, p, "MediaTrack*")) SetTrackSelected(p, true);
            UpdateArrange();
            return Json{{"ok", true}, {"fxCount", TrackFX_GetCount(t)},
                        {"itemCount", CountTrackMediaItems(t)}};
#else
            (void)idx;
            return Json{{"ok", true}, {"fxCount", 0}, {"itemCount", 0}};
#endif
        }});

    // ---- track.get_peak (read-only; live meter readback) ----
    reg.add(Tool{
        "track.get_peak",
        "Read a track's live peak meter per channel (linear, 1.0 = 0 dBFS, plus dB). Meaningful "
        "while audio is playing/monitoring; silent tracks report -inf (-150 dB floor). Pass "
        "'channel' for one channel, else all of the track's channels are returned.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "channel":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"channels":{"type":"integer"},
            "peaks":{"type":"array","items":{"type":"object","properties":{"channel":{"type":"integer"},
                "peak":{"type":"number"},"peakDb":{"type":"number"}}}}},
            "required":["track","channels","peaks"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool hasChan = a.contains("channel") && a["channel"].is_number();
            const int chan = optInt(a, "channel", 0);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            Json arr = Json::array();
            const int from = hasChan ? chan : 0;
            const int to = hasChan ? chan + 1 : nch;
            for (int c = from; c < to; ++c) {
                const double pk = Track_GetPeakInfo(t, c);
                arr.push_back(Json{{"channel", c}, {"peak", pk}, {"peakDb", gainToDb(pk)}});
            }
            return Json{{"track", idx}, {"channels", nch}, {"peaks", std::move(arr)}};
#else
            Json arr = Json::array();
            arr.push_back(Json{{"channel", hasChan ? chan : 0}, {"peak", 0.0}, {"peakDb", -150.0}});
            return Json{{"track", idx}, {"channels", 2}, {"peaks", std::move(arr)}};
#endif
        }});

    // =====================================================================================
    // Client-facing undo history verbs. Distinct from the per-verb undo blocks
    // every mutating tool already wraps its edit in — these drive REAPER's global undo stack so an
    // agent can walk history. Mutating (they change project state) and non-idempotent.
    // =====================================================================================

    // ---- edit.undo ----
    reg.add(Tool{
        "edit.undo", "Undo the most recent undoable action (REAPER's global undo stack).",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"undone":{"type":"string"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const char* name = Undo_CanUndo2(kCur);
            std::string undone = name ? name : "";
            const bool ok = Undo_DoUndo2(kCur) != 0;
            return Json{{"ok", ok}, {"undone", undone}};
#else
            return Json{{"ok", true}, {"undone", ""}};
#endif
        }});

    // ---- edit.redo ----
    reg.add(Tool{
        "edit.redo", "Redo the most recently undone action (REAPER's global undo stack).",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"redone":{"type":"string"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const char* name = Undo_CanRedo2(kCur);
            std::string redone = name ? name : "";
            const bool ok = Undo_DoRedo2(kCur) != 0;
            return Json{{"ok", ok}, {"redone", redone}};
#else
            return Json{{"ok", true}, {"redone", ""}};
#endif
        }});

    // ---- edit.get_undo_state (read-only) ----
    reg.add(Tool{
        "edit.get_undo_state", "Report whether undo/redo are available and the name of each next step.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"canUndo":{"type":"boolean"},
            "undoName":{"type":"string"},"canRedo":{"type":"boolean"},"redoName":{"type":"string"}},
            "required":["canUndo","canRedo"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const char* u = Undo_CanUndo2(kCur);
            const char* r = Undo_CanRedo2(kCur);
            return Json{{"canUndo", u != nullptr}, {"undoName", u ? u : ""},
                        {"canRedo", r != nullptr}, {"redoName", r ? r : ""}};
#else
            return Json{{"canUndo", false}, {"undoName", ""},
                        {"canRedo", false}, {"redoName", ""}};
#endif
        }});

    // =====================================================================================
    // Selection get/set. Selection is transient UI state (not an undo point), so
    // like track.select these do not open an undo block. selection.set clears the relevant axis
    // before applying an explicit list, so the result is exactly the requested selection.
    // =====================================================================================

    // ---- selection.get (read-only) ----
    reg.add(Tool{
        "selection.get", "Return the currently selected tracks (indices) and media items "
                         "(track+item indices, GUID).",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"trackCount":{"type":"integer"},
            "itemCount":{"type":"integer"},"tracks":{"type":"array","items":{"type":"integer"}},
            "items":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"integer"},"item":{"type":"integer"},"guid":{"type":"string"}}}}},
            "required":["trackCount","itemCount","tracks","items"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            Json tarr = Json::array();
            const int nt = CountSelectedTracks(kCur);
            for (int i = 0; i < nt; ++i) {
                MediaTrack* t = GetSelectedTrack(kCur, i);
                if (t) tarr.push_back(CSurf_TrackToID(t, false) - 1);
            }
            Json iarr = Json::array();
            const int ni = CountSelectedMediaItems(kCur);
            for (int i = 0; i < ni; ++i) {
                MediaItem* it = GetSelectedMediaItem(kCur, i);
                if (!it) continue;
                MediaTrack* tr = GetMediaItemTrack(it);
                int trackIdx = tr ? (CSurf_TrackToID(tr, false) - 1) : -1;
                int itemIdx = -1;
                if (tr) {
                    const int cnt = CountTrackMediaItems(tr);
                    for (int k = 0; k < cnt; ++k)
                        if (GetTrackMediaItem(tr, k) == it) { itemIdx = k; break; }
                }
                char gb[64] = {0};
                GetSetMediaItemInfo_String(it, "GUID", gb, false);
                iarr.push_back(Json{{"track", trackIdx}, {"item", itemIdx}, {"guid", gb}});
            }
            return Json{{"trackCount", (int)tarr.size()}, {"itemCount", (int)iarr.size()},
                        {"tracks", std::move(tarr)}, {"items", std::move(iarr)}};
#else
            return Json{{"trackCount", 0}, {"itemCount", 0},
                        {"tracks", Json::array()}, {"items", Json::array()}};
#endif
        }});

    // ---- selection.set (mutating UI state; idempotent) ----
    reg.add(Tool{
        "selection.set", "Set the track and/or item selection. deselectAll clears both axes first; "
                         "providing tracks or items replaces that axis with exactly the given set. "
                         "items are {track,item} index pairs.",
        jparse(R"({"type":"object","properties":{
            "deselectAll":{"type":"boolean","default":false},
            "tracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "items":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"integer","minimum":0},"item":{"type":"integer","minimum":0}},
                "required":["track","item"]}}},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "selectedTracks":{"type":"integer"},"selectedItems":{"type":"integer"}},
            "required":["ok","selectedTracks","selectedItems"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const bool deselectAll = optBool(a, "deselectAll", false);
            const bool hasTracks = a.contains("tracks") && a["tracks"].is_array();
            const bool hasItems = a.contains("items") && a["items"].is_array();
#ifdef REAPER_MCP_HAVE_SDK
            if (deselectAll || hasTracks) {
                const int n = CountTracks(kCur);
                for (int i = 0; i < n; ++i) { MediaTrack* t = GetTrack(kCur, i); if (t) SetTrackSelected(t, false); }
            }
            if (deselectAll || hasItems) SelectAllMediaItems(kCur, false);
            if (hasTracks)
                for (const auto& jt : a["tracks"])
                    if (jt.is_number()) { MediaTrack* t = GetTrack(kCur, jt.get<int>()); if (t) SetTrackSelected(t, true); }
            if (hasItems)
                for (const auto& ji : a["items"]) {
                    const int tr = ji.value("track", -1), im = ji.value("item", -1);
                    if (tr < 0 || im < 0) continue;
                    MediaTrack* t = GetTrack(kCur, tr);
                    if (!t) continue;
                    MediaItem* it = GetTrackMediaItem(t, im);
                    if (it) SetMediaItemSelected(it, true);
                }
            UpdateArrange();
            return Json{{"ok", true}, {"selectedTracks", CountSelectedTracks(kCur)},
                        {"selectedItems", CountSelectedMediaItems(kCur)}};
#else
            const int nt = hasTracks ? (int)a["tracks"].size() : 0;
            const int ni = hasItems ? (int)a["items"].size() : 0;
            return Json{{"ok", true}, {"selectedTracks", deselectAll ? 0 : nt},
                        {"selectedItems", deselectAll ? 0 : ni}};
#endif
        }});
}

}  // namespace reaper_mcp
