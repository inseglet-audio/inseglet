// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_actions.cpp — REAPER action / command-execution passthrough.
//
// `action.run` / `action.run_by_name` / `action.get_toggle_state`
// give an agent reach over the entire REAPER action list — native actions, SWS/js_ReaScriptAPI
// extensions, and the user's own custom actions/ReaScripts — without us wrapping each one.
//
// Safety model (see SECURITY.md + actions_policy.h). The trust boundary is the loopback
// bearer token, NOT this policy; the policy is a best-effort *pump-safety net*:
//   (a) every run is wrapped in its own named undo block on the main-thread queue (best-effort — REAPER
//       marks not-all-actions undoable; a mistaken run is one Cmd-Z away when it is);
//   (b) a built-in deny-list (numeric + display-name) refuses known modal/dialog-spawning or
//       process-fatal actions — a modal dialog would block the main thread that pumps our request
//       queue (the dialog-hangs-the-pump + App-Nap gotchas), and Quit would kill the server;
//   (c) an OPTIONAL allow-list (reaper_mcp_actions.json in the REAPER resource dir) for locked-down
//       deployments: empty => allow-all-except-deny (default); populated => allow-list ONLY.
//
// Dual-path like the other tool TUs: native REAPER calls under REAPER_MCP_HAVE_SDK; a schema-valid
// representative path otherwise (the deny net's numeric + allow-list layers still run host-side, so the
// gate is protocol-testable without REAPER — the name layer needs a live action list).

#include <fstream>
#include <stdexcept>
#include <string>

#include "tool_helpers.h"          // reqInt/reqStr/... + reaper_api.h (SWELL de-fang) + tool_registry.h
#include "../actions_policy.h"     // SDK-free deny-net + allow-list policy (unit-tested)
#include "../composite_support.h"  // makeError (structured, remediable error objects)

namespace reaper_mcp {
namespace {

// Hint appended to every action_denied error so an agent knows the (deliberate) escape hatch.
constexpr const char* kOverrideHint =
    "This action is blocked to keep the server responsive. If you genuinely need it, add its command "
    "id or name to actionsAllowList in reaper_mcp_actions.json (REAPER resource dir).";

#ifdef REAPER_MCP_HAVE_SDK
// The main action section (uniqueID 0) — needed to resolve an action's display name for the name layer.
KbdSectionInfo* mainSection() {
    static KbdSectionInfo* s = SectionFromUniqueID(0);
    return s;
}
// The action's REAPER display text (e.g. "File: Quit REAPER (no save prompt)"), or "" if unavailable.
std::string actionDisplayName(int cmd) {
    KbdSectionInfo* sec = mainSection();
    if (!sec) return std::string();
    const char* t = kbd_getTextFromCmd(cmd, sec);
    return t ? std::string(t) : std::string();
}
// The optional allow-list, loaded ONCE from reaper_mcp_actions.json in the resource dir. A missing or
// malformed file leaves it empty => default allow-all-except-deny mode.
const actions::PolicyConfig& policyConfig() {
    static const actions::PolicyConfig cfg = [] {
        actions::PolicyConfig c;
        const std::string path = std::string(GetResourcePath()) + "/reaper_mcp_actions.json";
        std::ifstream f(path);
        if (f) {
            try {
                Json j;
                f >> j;
                if (j.contains("allowCommandIds") && j["allowCommandIds"].is_array())
                    for (const auto& v : j["allowCommandIds"])
                        if (v.is_number_integer()) c.allowIds.insert(v.get<int>());
                if (j.contains("allowNames") && j["allowNames"].is_array())
                    for (const auto& v : j["allowNames"])
                        if (v.is_string()) c.allowNames.push_back(v.get<std::string>());
            } catch (...) {
                // Malformed config => ignore it and fall back to the safe default (deny-net mode).
            }
        }
        return c;
    }();
    return cfg;
}
#else
// Host path (no REAPER): the name layer can't resolve, and there is no config file.
std::string actionDisplayName(int) { return std::string(); }
const actions::PolicyConfig& policyConfig() {
    static const actions::PolicyConfig cfg;  // empty => deny-net mode
    return cfg;
}
#endif

// Shared run path for both action.run and action.run_by_name: resolve the display name, apply the
// policy, then (if allowed and not a dry run) run inside one named undo block and report the post-run
// toggle state. dryRun returns the resolved name + the policy verdict WITHOUT executing anything — a
// safe way for an agent (or the live gate) to check what an id maps to and whether it would be blocked.
Json runActionResolved(int cmd, const std::string& resolvedFrom, bool dryRun) {
    const std::string name = actionDisplayName(cmd);
    const actions::Decision d = actions::evaluate(cmd, name, policyConfig());

    if (dryRun) {
        Json r{{"dryRun", true}, {"command", cmd}, {"name", name}, {"wouldRun", d.allowed}};
        if (!d.allowed) r["reason"] = d.reason;
        if (!resolvedFrom.empty()) r["resolvedFrom"] = resolvedFrom;
        return r;
    }
    if (!d.allowed) return makeError("action_denied", d.reason, kOverrideHint);

#ifdef REAPER_MCP_HAVE_SDK
    Undo_BeginBlock2(kCur);
    Main_OnCommand(cmd, 0);
    const std::string desc =
        "MCP: run action " + std::to_string(cmd) + (name.empty() ? "" : " (" + name + ")");
    Undo_EndBlock2(kCur, desc.c_str(), -1);
    const int toggle = GetToggleCommandState(cmd);
#else
    const int toggle = -1;
#endif
    Json r{{"ok", true}, {"command", cmd}, {"name", name}, {"toggleState", toggle}};
    if (!resolvedFrom.empty()) r["resolvedFrom"] = resolvedFrom;
    return r;
}

}  // namespace

void registerActionTools(ToolRegistry& reg) {
    // ---- action.run (mutating; runs a numeric main-section command id) ----
    reg.add(Tool{
        "action.run",
        "Run a REAPER action by its numeric main-section command ID (from the Actions window), wrapped "
        "in one undo block. Blocked for known modal/fatal actions (see actionsAllowList). Returns the "
        "action's post-run toggle state (-1 = not a toggle).",
        jparse(R"({"type":"object","properties":{
            "command":{"type":"integer","description":"numeric main-section command ID"},
            "dryRun":{"type":"boolean","description":"resolve + policy-check only; run nothing"}},
            "required":["command"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"command":{"type":"integer"},
            "name":{"type":"string"},"toggleState":{"type":"integer"},"dryRun":{"type":"boolean"},
            "wouldRun":{"type":"boolean"},"reason":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"}},"required":["command"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int cmd = reqInt(a, "command");
            return runActionResolved(cmd, std::string(), optBool(a, "dryRun", false));
        }});

    // ---- action.run_by_name (mutating; resolves a named/_SWS_/_RS… command then runs it) ----
    reg.add(Tool{
        "action.run_by_name",
        "Resolve a NAMED REAPER command (e.g. an SWS action \"_SWS_...\" or a custom-action/ReaScript "
        "\"_RS...\" id) via NamedCommandLookup, then run it under the same undo + deny-list rails as "
        "action.run. For plain numeric IDs use action.run.",
        jparse(R"({"type":"object","properties":{
            "name":{"type":"string","description":"named command id, e.g. _SWS_ABOUT"},
            "dryRun":{"type":"boolean","description":"resolve + policy-check only; run nothing"}},
            "required":["name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"command":{"type":"integer"},
            "name":{"type":"string"},"resolvedFrom":{"type":"string"},"toggleState":{"type":"integer"},
            "dryRun":{"type":"boolean"},"wouldRun":{"type":"boolean"},"reason":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"}},"required":[]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const std::string named = reqStr(a, "name");
            int cmd = 0;
#ifdef REAPER_MCP_HAVE_SDK
            cmd = NamedCommandLookup(named.c_str());
#else
            // Host path has no REAPER resolver; accept a numeric string only, so the transport +
            // policy gate stay protocol-testable (named lookup itself needs a running REAPER).
            try {
                cmd = std::stoi(named);
            } catch (...) {
                cmd = 0;
            }
#endif
            if (cmd == 0)
                return makeError("action_not_found", "no command resolved for name: " + named,
                                 "Pass an exact named command id (e.g. _SWS_ABOUT). Numeric IDs go to "
                                 "action.run.");
            return runActionResolved(cmd, named, optBool(a, "dryRun", false));
        }});

    // ---- action.get_toggle_state (read-only) ----
    reg.add(Tool{
        "action.get_toggle_state",
        "Read the toggle/on-off state of a main-section action by numeric command ID. "
        "toggleState: -1 = not a toggle / unavailable, 0 = off, 1 = on.",
        jparse(R"({"type":"object","properties":{
            "command":{"type":"integer","description":"numeric main-section command ID"}},
            "required":["command"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"command":{"type":"integer"},"name":{"type":"string"},
            "toggleState":{"type":"integer"},"available":{"type":"boolean"}},
            "required":["command","toggleState"]})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true}, Profile::Core,
        [](const Json& a) -> Json {
            const int cmd = reqInt(a, "command");
            const std::string name = actionDisplayName(cmd);
#ifdef REAPER_MCP_HAVE_SDK
            const int st = GetToggleCommandState(cmd);
#else
            const int st = -1;
#endif
            return Json{{"command", cmd}, {"name", name}, {"toggleState", st}, {"available", st >= 0}};
        }});
}

}  // namespace reaper_mcp
