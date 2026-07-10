// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_actions.cpp — pure unit test for the Phase-8 (P8-2) command-execution safety policy
// (src/actions_policy.h): the numeric deny-set, the display-name deny layer, and the optional
// allow-list (default deny-net mode vs locked-down allow-list-only mode). No REAPER, no SDK.

#include <cstdio>
#include <string>

#include "actions_policy.h"

using namespace reaper_mcp;
using namespace reaper_mcp::actions;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

int main() {
    const PolicyConfig kDefault;  // empty => allow-all-except-deny (default mode)

    // ---- 1. default mode: ordinary actions run ----
    {
        check(evaluate(1007, "Transport: Play", kDefault).allowed, "benign action (Play) allowed");
        check(evaluate(40044, "Transport: Play/stop", kDefault).allowed, "benign action w/ name allowed");
        check(evaluate(12345, "", kDefault).allowed, "unknown id, no name -> allowed (default net)");
    }

    // ---- 2. numeric deny layer (locale-independent) ----
    {
        Decision d = evaluate(40860, "", kDefault);  // Close project, verified id — no name needed
        check(!d.allowed, "40860 (Close project) denied by numeric layer even without a name");
        check(d.reason.find("deny-list") != std::string::npos, "numeric denial reason mentions deny-list");
        check(builtinDeniedCommandIds().count(40860) == 1, "40860 is in the built-in numeric deny-set");
    }

    // ---- 3. name deny layer (version-robust; English display names) ----
    {
        check(!evaluate(99001, "File: Quit REAPER", kDefault).allowed, "Quit REAPER denied by name");
        check(!evaluate(99002, "File: Quit REAPER (no save prompt)", kDefault).allowed,
              "Quit variant denied by name");
        check(!evaluate(99003, "File: Save project as...", kDefault).allowed, "Save-as denied by name");
        check(!evaluate(99004, "File: Render project to disk...", kDefault).allowed,
              "Render-to-disk dialog denied by name");
        check(!evaluate(99005, "Options: Show REAPER preferences", kDefault).allowed,
              "Preferences denied by name");
        check(!evaluate(99006, "File: Open project...", kDefault).allowed, "Open project denied by name");
        // case-insensitive
        check(!evaluate(99007, "FILE: QUIT REAPER", kDefault).allowed, "name match is case-insensitive");
    }

    // ---- 4. name deny layer does NOT over-match routine edits ----
    {
        check(evaluate(99100, "Track: Render/freeze tracks to stereo stem (and mute originals)", kDefault)
                  .allowed,
              "non-dialog 'Render/freeze tracks' NOT denied (specific 'render project to disk' phrase)");
        check(evaluate(99101, "Item: Split items at edit cursor", kDefault).allowed,
              "Split items allowed");
        check(evaluate(99102, "Item: Open items in editor", kDefault).allowed, "generic edit allowed");
        check(evaluate(99103, "Track: Save tracks as track template", kDefault).allowed,
              "'save tracks as template' not caught by 'save project as'");
    }

    // ---- 5. allow-list mode (locked-down): only listed actions run; unlisted denied ----
    {
        PolicyConfig cfg;
        cfg.allowIds.insert(1007);
        cfg.allowNames.push_back("Transport:");  // allow the whole Transport family by name
        check(cfg.allowListActive(), "config with entries is active");

        check(evaluate(1007, "Transport: Play", cfg).allowed, "allow-listed id runs");
        check(evaluate(40073, "Transport: Pause", cfg).allowed, "allow-listed-by-name family runs");
        Decision d = evaluate(40012, "Item: Normalize items", cfg);
        check(!d.allowed, "unlisted action denied in allow-list mode");
        check(d.reason.find("actionsAllowList") != std::string::npos,
              "allow-list denial names actionsAllowList");
    }

    // ---- 6. allow-list overrides the deny net (operator vetted it) ----
    {
        PolicyConfig cfg;
        cfg.allowIds.insert(40860);  // operator explicitly permits Close project
        check(evaluate(40860, "File: Close current project", cfg).allowed,
              "explicitly allow-listed action bypasses the deny net");
    }

    if (g_failures) { std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures); return 1; }
    std::fprintf(stderr, "ALL ACTIONS-POLICY CHECKS PASSED\n");
    return 0;
}
