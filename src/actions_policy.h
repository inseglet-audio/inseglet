// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// actions_policy.h — command-execution safety policy (SDK-free, unit-tested).
//
// The `action.*` tools let an agent run REAPER actions by command ID / named command. The trust
// boundary is the loopback bearer token (SECURITY.md: single local user, the token is the gate).
// This policy is NOT that boundary — it is a best-effort *safety net* that keeps a well-meaning
// agent from misusing the single-threaded server:
//
//   * process-FATAL actions (Quit REAPER) would kill the server outright;
//   * MODAL/dialog-spawning actions block the main thread inside Main_OnCommand until a *human*
//     dismisses the dialog — and the main thread is exactly the pump that drains our request queue
//     (the "dialog-hangs-the-pump" + App-Nap gotchas). No client could recover it.
//
// Two layers, both here so the *logic* is host-testable without REAPER:
//   1. A locale-independent numeric deny-set of specific known-dangerous command IDs.
//   2. A name-substring deny-list matched against the action's REAPER display name (resolved at call
//      time via kbd_getTextFromCmd). Version-robust (IDs drift across builds; names rarely do) and the
//      broader net — NOTE it assumes English action names; a localized REAPER falls back to layer 1 +
//      the token boundary. Phrases are chosen specifically to avoid catching routine editing actions.
//
// Config: an OPTIONAL allow-list for locked-down deployments. Empty => allow-all-except the
// deny net (the default). Non-empty => allow-list ONLY: nothing runs unless it is explicitly listed
// (the operator has vetted those, so an allow-listed action bypasses the deny net). See tools_actions.cpp
// for where the allow-list is loaded (an optional reaper_mcp_actions.json in the resource dir).

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace actions {

// Lowercase a copy (ASCII) — local so this header stays free of any other include.
inline std::string toLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Layer 1 — locale-independent numeric deny-set. Deliberately SMALL: only IDs verified against a
// source of truth are hard-coded (a wrong ID would silently block an innocuous action). The name
// layer carries the breadth. Extend as more IDs are confirmed on the live rig.
//   40860 — "Close current project (and its tab)" — prompts to save / discards the session.
inline const std::set<int>& builtinDeniedCommandIds() {
    static const std::set<int> ids = {
        40860,  // File: Close current project — prompts to save / discards the session
    };
    return ids;
}

// Layer 2 — case-insensitive substrings matched against the action's display name. Each phrase is
// specific enough not to fire on ordinary edits (e.g. "render project to disk" catches the Render
// dialog but not "Track: Render/freeze tracks to stereo stems", which runs without a dialog).
inline const std::vector<std::string>& deniedNameSubstrings() {
    static const std::vector<std::string> pats = {
        "quit reaper",              // process-fatal — kills the server
        "close project",            // modal save-prompt / discards session
        "close all project",        // "
        "close all, no save",       // "
        "save project as",          // modal Save-As file dialog
        "save all projects",        // may prompt per project
        "save live output",         // opens the record/bounce config dialog
        "open project",             // modal Open file dialog
        "recent project",           // opens/loads via a menu/dialog
        "render project to disk",   // modal Render dialog
        "consolidate/export",       // modal Export dialog
        "export project",           // "
        "insert media file",        // modal file browser
        "show reaper preferences",  // modal Preferences
        "options: preferences",     // "
        "reaper preferences",       // "
    };
    return pats;
}

// The optional allow-list (empty by default => allow-all-except-deny). Populated from server config.
struct PolicyConfig {
    std::set<int> allowIds;              // allow-list by numeric command id
    std::vector<std::string> allowNames; // allow-list by (case-insensitive) display-name substring
    bool allowListActive() const { return !allowIds.empty() || !allowNames.empty(); }
};

struct Decision {
    bool allowed = true;
    std::string reason;  // populated only when !allowed
};

// True if `name` (the action's display text; may be empty if unresolved) contains any substring in
// `pats`, matched case-insensitively.
inline bool nameMatchesAny(const std::string& name, const std::vector<std::string>& pats) {
    if (name.empty() || pats.empty()) return false;
    const std::string ln = toLowerAscii(name);
    for (const std::string& p : pats)
        if (!p.empty() && ln.find(toLowerAscii(p)) != std::string::npos) return true;
    return false;
}

// Decide whether a resolved action may run. `name` is the action's REAPER display name (empty if the
// caller couldn't resolve it — then only the numeric layer / allow-list apply).
inline Decision evaluate(int cmdId, const std::string& name, const PolicyConfig& cfg) {
    // Allow-list mode: nothing runs unless explicitly listed. An operator who curated the list has
    // vetted those actions, so a listed action is permitted even if it would otherwise be denied.
    if (cfg.allowListActive()) {
        const bool listed =
            cfg.allowIds.count(cmdId) > 0 || nameMatchesAny(name, cfg.allowNames);
        if (!listed)
            return {false,
                    "action is not in the configured actionsAllowList (locked-down deployment)"};
        return {true, {}};
    }

    // Default mode: allow everything except the built-in deny net.
    if (builtinDeniedCommandIds().count(cmdId) > 0)
        return {false, "command id " + std::to_string(cmdId) +
                           " is on the built-in deny-list (modal/fatal action — would block the "
                           "main-thread pump or quit REAPER)"};
    if (nameMatchesAny(name, deniedNameSubstrings()))
        return {false, "action \"" + name +
                           "\" matches a deny pattern (modal/fatal action — would block the "
                           "main-thread pump or quit REAPER; override via actionsAllowList if intended)"};
    return {true, {}};
}

}  // namespace actions
}  // namespace reaper_mcp
