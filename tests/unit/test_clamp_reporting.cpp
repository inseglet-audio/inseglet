// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_clamp_reporting.cpp — a mutating tool must report the input it silently adjusted.
//
// `track.set_channels` adjusts its input three ways (clamp low, clamp high, round odd up) and
// used to return `{"ok": true, "channels": N}` with nothing naming the adjustment, so a caller
// that trusts `ok` cannot see that it did not get what it asked for.
//
// The project already ships the convention this asserts: the `spatial.*` tools push a
// human-readable line into a `warnings` array, and the accessor tools publish a `clamped`
// boolean in their declared output schema.  This test asserts that ONE mutating tool now
// carries it too — and, just as importantly, that it says NOTHING when nothing was adjusted.
//
// Runs on the SDK-free host-core path, so it needs no REAPER.

#include <cstdio>
#include <string>

#include "tool_registry.h"

using namespace reaper_mcp;
using Json = nlohmann::json;

namespace reaper_mcp { void registerRoutingTools(ToolRegistry&); }

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

int main() {
    ToolRegistry tools;
    registerRoutingTools(tools);
    const Tool* t = tools.find("track.set_channels");
    if (!t || !t->handler) { std::fprintf(stderr, "FAIL: track.set_channels not registered\n"); return 1; }

    auto call = [&](int ch) -> Json {
        try { return t->handler(Json{{"track", 0}, {"channels", ch}}); }
        catch (const std::exception& e) { return Json{{"_threw", e.what()}}; }
    };

    std::fprintf(stderr, "== the NEGATIVE control first: a field that is always true reports nothing ==\n");
    for (int ok : {2, 12, 64}) {
        const Json r = call(ok);
        check(r.value("channels", -1) == ok, "in-range " + std::to_string(ok) + " is returned unchanged");
        check(r.contains("clamped") && r["clamped"].get<bool>() == false,
              "in-range " + std::to_string(ok) + " reports clamped:false");
        check(r.contains("warnings") && r["warnings"].is_array() && r["warnings"].empty(),
              "in-range " + std::to_string(ok) + " carries an EMPTY warnings array");
    }

    std::fprintf(stderr, "== every adjustment the handler makes must be reported ==\n");
    struct Case { int asked, got; const char* why; };
    for (const Case& c : {Case{1, 2, "clamp low"}, Case{0, 2, "clamp low"}, Case{-5, 2, "clamp low"},
                          Case{3, 4, "round odd up"}, Case{13, 14, "round odd up"},
                          Case{65, 64, "clamp high"}, Case{100, 64, "clamp high"},
                          Case{99, 64, "clamp high before rounding"}}) {
        const Json r = call(c.asked);
        const std::string tag = std::to_string(c.asked) + " -> " + std::to_string(c.got);
        check(r.value("channels", -1) == c.got, tag + " (" + c.why + ")");
        check(r.value("ok", false) == true, tag + " still reports ok:true");
        check(r.value("clamped", false) == true, tag + " reports clamped:TRUE");
        check(r.contains("warnings") && r["warnings"].is_array() && !r["warnings"].empty(),
              tag + " carries a non-empty warnings array");
        if (r.contains("warnings") && r["warnings"].is_array() && !r["warnings"].empty()) {
            const std::string w = r["warnings"][0].get<std::string>();
            check(w.find(std::to_string(c.asked)) != std::string::npos,
                  tag + " the warning names what was ASKED FOR");
            check(w.find(std::to_string(c.got)) != std::string::npos,
                  tag + " the warning names what was GIVEN");
        }
    }

    std::fprintf(stderr, "== the declared output schema must admit the new fields ==\n");
    check(t->outputSchema.is_object() && t->outputSchema.contains("properties"),
          "outputSchema is an object with properties");
    if (t->outputSchema.contains("properties")) {
        const Json& p = t->outputSchema["properties"];
        check(p.contains("clamped"), "outputSchema declares `clamped`");
        check(p.contains("warnings"), "outputSchema declares `warnings`");
    }

    std::fprintf(stderr, g_failures ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
