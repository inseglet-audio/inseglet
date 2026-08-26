// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_adjust_reporting.cpp — the fourteen that followed `track.set_channels`.
//
// The sibling test (test_clamp_reporting.cpp) asserts ONE tool. This asserts the rest of the
// mutating surface, and it draws a line worth drawing:
//
//   NINE INPUT CLAMPERS — a value the caller supplied is changed before it is used. The report
//   names what was ASKED FOR and what was USED, because those differ.
//
//   FIVE PER-NOTE PINS (midi.*) — nothing the caller typed is changed. The argument is honoured
//   exactly, and N notes still cannot land where it implies, so they pin to 0. Reporting these
//   as "requested X, used Y" would be FALSE. They report a COUNT.
//
// ⛔ THE NEGATIVE CONTROL IS THE LOAD-BEARING HALF. A tool that returns clamped:true for
//    everything would pass every positive assertion here and be useless. Every tool is called
//    first with an in-range value and must report clamped:false with an EMPTY warnings array.
//
// Runs on the SDK-free host-core path, so it needs no REAPER. ⚠️ That path cannot execute the
// midi note loop, so the five pins are asserted here for CONTRACT ONLY — the fields exist, are
// correctly typed, and read false/empty when nothing was adjusted. Their pinning behaviour is
// verified LIVE on the shipped binary, and a green run of this file is not a claim about it.

#include <cstdio>
#include <string>
#include <vector>

#include "tool_registry.h"

using namespace reaper_mcp;
using Json = nlohmann::json;

namespace reaper_mcp {
void registerCoreTools(ToolRegistry&);
void registerItemTools(ToolRegistry&);
void registerMidiTools(ToolRegistry&);
}  // namespace reaper_mcp

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Every tool asserted here must declare BOTH fields as required — the declaration is the
// only contract this server has, since nothing validates a declared schema.
static void checkDeclared(const Tool* t, const std::string& name) {
    const Json& os = t->outputSchema;
    bool req = false;
    if (os.contains("required") && os["required"].is_array()) {
        int hits = 0;
        for (const auto& r : os["required"])
            if (r == "clamped" || r == "warnings") ++hits;
        req = (hits == 2);
    }
    check(req, name + " DECLARES clamped and warnings as required");
}

static void quiet(const Tool* t, const std::string& name, const Json& args) {
    Json r;
    try { r = t->handler(args); }
    catch (const std::exception& e) { check(false, name + " in-range threw: " + e.what()); return; }
    check(r.contains("clamped") && r["clamped"].is_boolean() && !r["clamped"].get<bool>(),
          name + " in-range reports clamped:FALSE");
    check(r.contains("warnings") && r["warnings"].is_array() && r["warnings"].empty(),
          name + " in-range carries an EMPTY warnings array");
}

static void loud(const Tool* t, const std::string& name, const Json& args,
                 const std::string& asked, const std::string& used) {
    Json r;
    try { r = t->handler(args); }
    catch (const std::exception& e) { check(false, name + " out-of-range threw: " + e.what()); return; }
    check(r.value("ok", false), name + " out-of-range still reports ok:true");
    check(r.value("clamped", false), name + " out-of-range reports clamped:TRUE");
    const bool has = r.contains("warnings") && r["warnings"].is_array() && !r["warnings"].empty();
    check(has, name + " out-of-range carries a non-empty warnings array");
    if (!has) return;
    const std::string w = r["warnings"][0].get<std::string>();
    check(w.find(asked) != std::string::npos, name + " the warning names what was ASKED (" + asked + ")");
    check(w.find(used) != std::string::npos, name + " the warning names what was USED (" + used + ")");
}

int main() {
    ToolRegistry tools;
    registerCoreTools(tools);
    registerItemTools(tools);
    registerMidiTools(tools);

    struct Clamper {
        const char* name;
        Json inRange, low, high;          // `high` may be null where the field has no upper bound
        const char* lowAsked; const char* lowUsed;
        const char* highAsked; const char* highUsed;
    };
    const std::vector<Clamper> CLAMPERS = {
        {"transport.set_playrate", {{"rate", 1.0}}, {{"rate", 0.1}}, {{"rate", 9.0}},
         "0.1", "0.25", "9", "4"},
        {"track.set_rec_mon", {{"track", 0}, {"mode", 1}}, {{"track", 0}, {"mode", -3}},
         {{"track", 0}, {"mode", 7}}, "-3", "0", "7", "2"},
        {"track.set_rec_mode", {{"track", 0}, {"mode", 4}}, {{"track", 0}, {"mode", -1}},
         {{"track", 0}, {"mode", 99}}, "-1", "0", "99", "8"},
        {"track.set_folder", {{"track", 0}, {"depth", 0}, {"compact", 1}},
         {{"track", 0}, {"depth", 0}, {"compact", -4}},
         {{"track", 0}, {"depth", 0}, {"compact", 5}}, "-4", "0", "5", "2"},
        {"item.set_position", {{"track", 0}, {"item", 0}, {"position", 3.5}},
         {{"track", 0}, {"item", 0}, {"position", -2.0}}, Json(), "-2", "0", nullptr, nullptr},
        {"item.set_length", {{"track", 0}, {"item", 0}, {"length", 4.0}},
         {{"track", 0}, {"item", 0}, {"length", -1.0}}, Json(), "-1", "0", nullptr, nullptr},
        {"item.set_fade_in", {{"track", 0}, {"item", 0}, {"length", 0.5}},
         {{"track", 0}, {"item", 0}, {"length", -0.5}}, Json(), "-0.5", "0", nullptr, nullptr},
        {"item.set_fade_out", {{"track", 0}, {"item", 0}, {"length", 0.5}},
         {{"track", 0}, {"item", 0}, {"length", -0.5}}, Json(), "-0.5", "0", nullptr, nullptr},
        {"take.set_pan", {{"track", 0}, {"item", 0}, {"pan", 0.25}},
         {{"track", 0}, {"item", 0}, {"pan", -3.0}},
         {{"track", 0}, {"item", 0}, {"pan", 2.0}}, "-3", "-1", "2", "1"},
    };

    std::fprintf(stderr, "== NINE INPUT CLAMPERS ==\n");
    for (const Clamper& c : CLAMPERS) {
        const Tool* t = tools.find(c.name);
        if (!t || !t->handler) { check(false, std::string(c.name) + " is registered"); continue; }
        std::fprintf(stderr, "-- %s\n", c.name);
        checkDeclared(t, c.name);
        quiet(t, c.name, c.inRange);                       // the negative control, FIRST
        loud(t, c.name, c.low, c.lowAsked, c.lowUsed);
        if (!c.high.is_null()) loud(t, c.name, c.high, c.highAsked, c.highUsed);
    }

    std::fprintf(stderr, "== FIVE PER-NOTE PINS (contract only on this path) ==\n");
    const std::vector<std::pair<const char*, Json>> PINS = {
        {"midi.quantize",     Json{{"track", 0}, {"item", 0}}},
        {"midi.humanize",     Json{{"track", 0}, {"item", 0}}},
        {"midi.apply_groove", Json{{"track", 0}, {"item", 0}, {"groove", "swing8"}, {"amount", 0.33}}},
        {"midi.nudge",        Json{{"track", 0}, {"item", 0}, {"beats", 1.0}}},
        {"midi.stretch",      Json{{"track", 0}, {"item", 0}, {"factor", 2.0}}},
    };
    for (const auto& p : PINS) {
        const Tool* t = tools.find(p.first);
        if (!t || !t->handler) { check(false, std::string(p.first) + " is registered"); continue; }
        std::fprintf(stderr, "-- %s\n", p.first);
        checkDeclared(t, p.first);
        quiet(t, p.first, p.second);
    }

    std::fprintf(stderr, "== the tool this item does NOT touch ==\n");
    // spatial.set_source_position is flagged by our silent-adjustment audit and is a FALSE
    // POSITIVE: its only match is `if (fx < 0) fx = findPositionalFx(t).fx;`, an FX-index
    // lookup fallback, not an input adjustment. It is NOT repaired and is NOT asserted here.
    // Making an audit's count reach zero by "fixing" a tool with no defect would be worse
    // than the count.
    std::fprintf(stderr, "  (a known false positive — deliberately unasserted)\n");

    std::fprintf(stderr, g_failures ? "\nFAILURES: %d\n" : "\nall assertions passed (%d failures)\n",
                 g_failures);
    return g_failures ? 1 : 0;
}
