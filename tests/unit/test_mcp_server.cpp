// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_mcp_server.cpp — host-side MCP protocol test (no REAPER required).
//
// Stands up the real McpServer with the real core+spatial tool registry, drives a background thread
// that drains the MainThreadQueue (standing in for REAPER's main-thread Run() pump), and exercises
// the transport over actual HTTP with an MCP-style client:
//   initialize -> tools/list -> tools/call (read + mutate + error) -> ping -> auth rejection.
//
// It asserts JSON-RPC framing, structured-output shape (against each tool's declared fields), the
// main-thread hop, and bearer-token enforcement. The tool bodies run in their SDK-free path, so the
// values are representative but the protocol contract is exercised for real.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "composite_support.h"  // Phase 6 (d): requireElicitation / makeError for the test verb
#include "track_chunk.h"         // Phase 8 (D3): validateTrackChunk / diffTrackChunks unit checks
#include "discovery.h"
#include "main_thread_queue.h"
#include "mcp_server.h"
#include "prompt_registry.h"
#include "resource_registry.h"
#include "subscription_hub.h"
#include "tool_registry.h"

using nlohmann::json;
using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "  FAIL: %s\n", what.c_str());
        ++g_failures;
    } else {
        std::fprintf(stderr, "  ok:   %s\n", what.c_str());
    }
}

// POST a JSON-RPC request and return the parsed response body (or a discarded json on transport err).
static json rpc(httplib::Client& cli, const std::string& token, const json& req, int* statusOut = nullptr) {
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto res = cli.Post("/mcp", h, req.dump(), "application/json");
    if (!res) {
        if (statusOut) *statusOut = -1;
        return json::value_t::discarded;
    }
    if (statusOut) *statusOut = res->status;
    return json::parse(res->body, nullptr, false);
}

int main() {
    // --- registry + main-thread pump ---
    ToolRegistry tools;
    registerCoreTools(tools);
    registerMetaTools(tools);
    registerMarkerTools(tools);
    registerItemTools(tools);
    registerFxTools(tools);
    registerRoutingTools(tools);
    registerEnvelopeTools(tools);
    registerTimelineTools(tools);
    registerMidiTools(tools);
    registerSpatialTools(tools);
    registerAnalysisTools(tools);   // Phase 5c
    registerMixTools(tools);        // Phase 5d
    registerSessionTools(tools);    // Phase 5e
    registerActionTools(tools);     // Phase 8 P8-2

    // Phase 6 (d): a test-only destructive verb that exercises the elicitation lane host-side. The
    // real destructive gate (setup_immersive_session) only fires under REAPER (it counts tracks), so
    // this stand-in lets the transport round-trip be proven over real HTTP in the cloud. It is NOT a
    // production tool — registered only in this harness; requireElicitation() throws when the call is
    // elicitation-capable (transport asks the human, re-invokes with confirm:true), else it no-ops and
    // this returns the confirm:true stopgap error.
    tools.add(Tool{
        "test.elicit_confirm",
        "Test-only destructive verb; confirms via MCP elicitation.",
        jparse(R"({"type":"object","properties":{"confirm":{"type":"boolean"},"note":{"type":"string"}}})"),
        jparse(R"({"type":"object"})"),
        ToolAnnotations{false, true, false}, Profile::Full,
        [](const Json& a) -> Json {
            const bool confirm = a.value("confirm", false);
            if (!confirm) {
                requireElicitation("test.elicit_confirm will apply a destructive change. Proceed?");
                return makeError("confirmation_required", "test.elicit_confirm needs confirmation",
                                 "re-run with confirm:true, or accept the elicitation prompt");
            }
            return Json{{"ok", true}, {"confirmed", true}, {"note", a.value("note", std::string())}};
        }});

    ResourceRegistry resources;
    registerProjectResources(resources);

    PromptRegistry prompts;
    registerExpertPrompts(prompts);

    SubscriptionHub subs;

    MainThreadQueue queue;
    std::atomic<bool> pump{true};
    std::thread mainThread([&] {
        while (pump.load()) {
            queue.drainMainThread();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.drainMainThread();  // final drain
    });

    // --- server ---
    ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;  // OS-assigned
    cfg.token = "test-token-abc123";
    cfg.elicitationTimeoutMs = 2000;  // Phase 6 (d): short so the timeout case is quick; accept/decline
                                      // answers arrive in ms, well under this.
    McpServer server(cfg, tools, resources, prompts, subs, queue);
    server.start();
    const uint16_t port = server.boundPort();
    check(port != 0, "server bound to an OS-assigned port");
    check(server.isRunning(), "server reports running");

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(6, 0);

    // --- 1. initialize ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                    {"params", {{"protocolVersion", "2025-06-18"},
                                {"capabilities", json::object()},
                                {"clientInfo", {{"name", "test"}, {"version", "0"}}}}}};
        int status = 0;
        json resp = rpc(cli, cfg.token, req, &status);
        check(status == 200, "initialize HTTP 200");
        check(resp.is_object() && resp.value("jsonrpc", "") == "2.0", "initialize is JSON-RPC 2.0");
        check(resp.contains("result"), "initialize has result");
        const json& r = resp["result"];
        check(r.value("protocolVersion", "") == "2025-06-18", "protocolVersion negotiated");
        check(r["serverInfo"].value("name", "") == "reaper_mcp", "serverInfo.name == reaper_mcp");
        check(r["capabilities"]["tools"].is_object(), "advertises tools capability");
        check(r["capabilities"]["resources"].value("subscribe", false), "advertises resource subscribe");
        check(r["capabilities"]["prompts"].is_object(), "advertises prompts capability");
    }

    // --- 2. tools/list ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}};
        json resp = rpc(cli, cfg.token, req);
        check(resp.contains("result"), "tools/list has result");
        const json& list = resp["result"]["tools"];
        check(list.is_array() && list.size() >= 30, "tools/list returns the full Phase-2 surface (>= 30)");

        bool sawGetState = false, sawAdd = false, sawSpatial = false, sawMeta = false;
        bool sawMarker = false, sawItem = false, sawFx = false, sawSend = false, sawTempo = false;
        bool sawMidiNote = false, sawTake = false, sawSession = false;
        for (const auto& t : list) {
            check(t.contains("name") && t.contains("description"), "tool has name+description");
            check(t["inputSchema"].is_object(), "tool inputSchema is an object (not a string)");
            check(t["annotations"].is_object(), "tool has annotations");
            const std::string n = t.value("name", "");
            if (n == "transport.get_state") {
                sawGetState = true;
                check(t["outputSchema"].is_object(), "transport.get_state has outputSchema object");
                check(t["annotations"].value("readOnlyHint", false), "get_state marked readOnly");
            }
            if (n == "track.add") sawAdd = true;
            if (n == "spatial.ambisonic_encode") sawSpatial = true;
            if (n == "tools.enumerate") sawMeta = true;
            if (n == "marker.add") sawMarker = true;
            if (n == "item.add") sawItem = true;
            if (n == "fx.add") sawFx = true;
            if (n == "send.add") sawSend = true;
            if (n == "tempo.get") sawTempo = true;
            if (n == "midi.insert_note") sawMidiNote = true;
            if (n == "take.list") sawTake = true;
            if (n == "session.run_dsl") {
                sawSession = true;
                check(t["annotations"].value("readOnlyHint", true) == false, "run_dsl marked mutating (not readOnly)");
            }
        }
        check(sawGetState, "transport.get_state present");
        check(sawAdd, "track.add present");
        check(sawSpatial, "spatial.ambisonic_encode present (spatial profile visible under full)");
        check(sawMeta, "tools.enumerate meta-tool present");
        check(sawMarker && sawItem && sawFx && sawSend && sawTempo,
              "Phase-2 parity tools present (marker/item/fx/send/tempo)");
        check(sawMidiNote && sawTake,
              "Phase-2b MIDI/take tools present (midi.insert_note / take.list)");
        check(sawSession, "session.run_dsl present (Phase-5e macro-DSL runner, full profile)");
    }

    // --- 2c. Phase-8 (P8-1) editing-breadth surface + representative host-path calls ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 810}, {"method", "tools/list"}, {"params", json::object()}};
        json resp = rpc(cli, cfg.token, req);
        const json& list = resp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        const char* p81[] = {
            "track.set_mute", "track.set_solo", "track.set_color", "track.set_folder",
            "track.set_rec_arm", "track.set_rec_input", "track.set_rec_mon", "track.set_phase",
            "track.set_width", "track.get", "item.set_position", "item.set_length", "item.set_mute",
            "item.set_vol", "item.set_fade_in", "item.set_fade_out", "item.set_color",
            "item.set_selected", "item.split", "item.delete", "item.move", "item.glue",
            "item.duplicate", "selection.get", "selection.set", "edit.undo", "edit.redo",
            "edit.get_undo_state", "region.list", "region.delete", "marker.goto", "region.goto",
            "marker.edit"};
        bool all = true; std::string missing;
        for (auto* n : p81) if (!listHas(n)) { all = false; missing = n; break; }
        check(all, all ? "all 33 P8-1 editing-breadth tools present in tools/list"
                       : ("P8-1 tool missing from tools/list: " + missing));
    }
    {
        // a granular setter round-trips its required value (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 811}, {"method", "tools/call"},
                    {"params", {{"name", "track.set_mute"}, {"arguments", {{"track", 0}, {"muted", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", true) == false, "track.set_mute not an error");
        check(resp["result"]["structuredContent"].value("muted", false) == true,
              "track.set_mute echoes muted=true");
    }
    {
        // the required value arg is enforced (missing 'muted' -> tool error, not a crash)
        json req = {{"jsonrpc", "2.0"}, {"id", 812}, {"method", "tools/call"},
                    {"params", {{"name", "track.set_mute"}, {"arguments", {{"track", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false),
              "track.set_mute without its value arg -> isError (D1 required-arg contract)");
    }
    {
        // an item granular setter echoes its field
        json req = {{"jsonrpc", "2.0"}, {"id", 813}, {"method", "tools/call"},
                    {"params", {{"name", "item.set_position"},
                                {"arguments", {{"track", 0}, {"item", 0}, {"position", 4.5}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].value("position", -1.0) == 4.5,
              "item.set_position echoes position (main-thread hop)");
    }
    {
        // selection.set structured shape, then the read-only undo-state verb
        json req = {{"jsonrpc", "2.0"}, {"id", 814}, {"method", "tools/call"},
                    {"params", {{"name", "selection.set"}, {"arguments", {{"deselectAll", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].value("ok", false), "selection.set deselectAll -> ok");

        json req2 = {{"jsonrpc", "2.0"}, {"id", 815}, {"method", "tools/call"},
                     {"params", {{"name", "edit.get_undo_state"}, {"arguments", json::object()}}}};
        json resp2 = rpc(cli, cfg.token, req2);
        const json& sc2 = resp2["result"]["structuredContent"];
        check(sc2.contains("canUndo") && sc2.contains("canRedo"),
              "edit.get_undo_state reports canUndo/canRedo");
    }

    // --- 2c-B2. Batch B2 breadth (groove/quantize + takes/comping + transport extras) ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 830}, {"method", "tools/list"}, {"params", json::object()}};
        json resp = rpc(cli, cfg.token, req);
        const json& list = resp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        const char* b2[] = {
            "midi.quantize", "midi.humanize", "midi.apply_groove", "midi.extract_groove",
            "midi.transpose", "midi.scale_velocity", "midi.legato", "midi.nudge", "midi.stretch",
            "take.add", "take.delete", "take.crop_to_active", "take.set_vol", "take.set_pan",
            "take.set_pitch", "take.set_playrate", "items.implode_to_takes",
            "transport.set_repeat", "transport.set_playrate", "transport.set_metronome"};
        bool all = true; std::string missing;
        for (auto* n : b2) if (!listHas(n)) { all = false; missing = n; break; }
        check(all, all ? "all 20 Batch B2 breadth tools present in tools/list"
                       : ("B2 tool missing from tools/list: " + missing));
    }
    {
        // transport.set_repeat echoes the loop flag (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 831}, {"method", "tools/call"},
                    {"params", {{"name", "transport.set_repeat"}, {"arguments", {{"enabled", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].value("loop", false) == true,
              "transport.set_repeat echoes loop=true (main-thread hop)");
    }
    {
        // take.add returns a take count (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 832}, {"method", "tools/call"},
                    {"params", {{"name", "take.add"}, {"arguments", {{"track", 0}, {"item", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", true) == false &&
              resp["result"]["structuredContent"].contains("takeCount"),
              "take.add returns takeCount");
    }
    {
        // midi.quantize host path resolves + returns 'changed'
        json req = {{"jsonrpc", "2.0"}, {"id", 833}, {"method", "tools/call"},
                    {"params", {{"name", "midi.quantize"},
                                {"arguments", {{"track", 0}, {"item", 0}, {"grid", 0.25}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", true) == false &&
              resp["result"]["structuredContent"].contains("changed"),
              "midi.quantize host path returns 'changed'");
    }
    {
        // midi.transpose enforces its required 'semitones' arg (missing -> isError)
        json req = {{"jsonrpc", "2.0"}, {"id", 834}, {"method", "tools/call"},
                    {"params", {{"name", "midi.transpose"}, {"arguments", {{"track", 0}, {"item", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false),
              "midi.transpose without 'semitones' -> isError (required-arg contract)");
    }
    {
        // midi.apply_groove requires a template or a named groove (neither -> isError)
        json req = {{"jsonrpc", "2.0"}, {"id", 835}, {"method", "tools/call"},
                    {"params", {{"name", "midi.apply_groove"}, {"arguments", {{"track", 0}, {"item", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false),
              "midi.apply_groove with neither template nor groove -> isError");
    }

    // --- 2d. Phase-8 (P8-2) actions passthrough: presence, run, deny-list gate, toggle-state ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 820}, {"method", "tools/list"}, {"params", json::object()}};
        json resp = rpc(cli, cfg.token, req);
        const json& list = resp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("action.run") && listHas("action.run_by_name") &&
                  listHas("action.get_toggle_state"),
              "all 3 P8-2 action tools present in tools/list");
    }
    {
        // a benign command runs (host path returns ok; SDK path runs it in one undo block)
        json req = {{"jsonrpc", "2.0"}, {"id", 821}, {"method", "tools/call"},
                    {"params", {{"name", "action.run"}, {"arguments", {{"command", 1007}}}}}};  // Play
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(resp["result"].value("isError", true) == false, "action.run(1007) not a transport error");
        check(sc.value("ok", false) && sc.value("command", -1) == 1007,
              "action.run echoes ok + command (main-thread hop)");
    }
    {
        // the D2 deny net refuses a known modal/fatal id (40860 = Close project) — structured error, no run
        json req = {{"jsonrpc", "2.0"}, {"id", 822}, {"method", "tools/call"},
                    {"params", {{"name", "action.run"}, {"arguments", {{"command", 40860}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("error", "") == "action_denied",
              "action.run(40860) blocked by the built-in deny-list (D2)");
    }
    {
        // dryRun resolves + policy-checks but runs nothing: a denied id reports wouldRun=false, a
        // benign id wouldRun=true — the safe check-before-run path.
        json req = {{"jsonrpc", "2.0"}, {"id", 8221}, {"method", "tools/call"},
                    {"params", {{"name", "action.run"}, {"arguments", {{"command", 40860}, {"dryRun", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("dryRun", false) && sc.value("wouldRun", true) == false,
              "action.run dryRun on a denied id -> wouldRun=false, nothing run");
        json req2 = {{"jsonrpc", "2.0"}, {"id", 8222}, {"method", "tools/call"},
                     {"params", {{"name", "action.run"}, {"arguments", {{"command", 1007}, {"dryRun", true}}}}}};
        json resp2 = rpc(cli, cfg.token, req2);
        check(resp2["result"]["structuredContent"].value("wouldRun", false),
              "action.run dryRun on a benign id -> wouldRun=true");
    }
    {
        // required-arg contract: missing 'command' -> tool error (throw -> isError), not a crash
        json req = {{"jsonrpc", "2.0"}, {"id", 823}, {"method", "tools/call"},
                    {"params", {{"name", "action.run"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false), "action.run without 'command' -> isError");
    }
    {
        // run_by_name: an unresolvable name -> structured action_not_found (host resolves numeric only)
        json req = {{"jsonrpc", "2.0"}, {"id", 824}, {"method", "tools/call"},
                    {"params", {{"name", "action.run_by_name"}, {"arguments", {{"name", "_NOT_A_COMMAND"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("error", "") == "action_not_found",
              "action.run_by_name(unknown) -> action_not_found");
    }
    {
        // run_by_name reaches the same deny gate once resolved (host numeric path: "40860" -> denied)
        json req = {{"jsonrpc", "2.0"}, {"id", 825}, {"method", "tools/call"},
                    {"params", {{"name", "action.run_by_name"}, {"arguments", {{"name", "40860"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("error", "") == "action_denied",
              "action.run_by_name resolves then hits the deny gate");
    }
    {
        // get_toggle_state read-only shape
        json req = {{"jsonrpc", "2.0"}, {"id", 826}, {"method", "tools/call"},
                    {"params", {{"name", "action.get_toggle_state"}, {"arguments", {{"command", 1007}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(resp["result"].value("isError", true) == false && sc.contains("toggleState") &&
                  sc.contains("available"),
              "action.get_toggle_state returns toggleState + available");
    }

    // --- 2e. Phase-8 (P8-3) depth: FX/take-FX, sends/routing, tempo markers, project, master ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 830}, {"method", "tools/list"}, {"params", json::object()}};
        json resp = rpc(cli, cfg.token, req);
        const json& list = resp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        const char* p83[] = {
            "fx.set_enabled", "fx.get_preset", "fx.set_preset",
            "takefx.add", "takefx.list", "takefx.remove", "takefx.get_param", "takefx.set_param",
            "takefx.set_enabled",
            "send.set", "send.set_channels", "receive.list", "send.add_hardware_output",
            "tempo.list_markers", "tempo.add_marker", "tempo.edit_marker", "tempo.delete_marker",
            "time.qn_to_time", "time.time_to_qn",
            "project.save", "project.open", "project.new", "project.render",
            "master.get", "master.set", "master.add_fx", "master.set_fx_param"};
        bool all = true; std::string missing;
        for (auto* n : p83) if (!listHas(n)) { all = false; missing = n; break; }
        check(all, all ? "all 27 P8-3 depth tools present in tools/list"
                       : ("P8-3 tool missing from tools/list: " + missing));
    }
    {
        // fx.set_enabled: required-arg contract + echo (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 831}, {"method", "tools/call"},
                    {"params", {{"name", "fx.set_enabled"},
                                {"arguments", {{"track", 0}, {"fx", 0}, {"enabled", false}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", true) == false &&
                  resp["result"]["structuredContent"].value("enabled", true) == false,
              "fx.set_enabled echoes enabled=false");
        json req2 = {{"jsonrpc", "2.0"}, {"id", 832}, {"method", "tools/call"},
                     {"params", {{"name", "fx.set_enabled"}, {"arguments", {{"track", 0}, {"fx", 0}}}}}};
        check(rpc(cli, cfg.token, req2)["result"].value("isError", false),
              "fx.set_enabled without 'enabled' -> isError");
    }
    {
        // fx.set_preset requires 'preset' OR 'move'
        json req = {{"jsonrpc", "2.0"}, {"id", 833}, {"method", "tools/call"},
                    {"params", {{"name", "fx.set_preset"}, {"arguments", {{"track", 0}, {"fx", 0}}}}}};
        check(rpc(cli, cfg.token, req)["result"].value("isError", false),
              "fx.set_preset without preset/move -> isError");
    }
    {
        // takefx.set_param normalized round-trips (host path echoes normalized)
        json req = {{"jsonrpc", "2.0"}, {"id", 834}, {"method", "tools/call"},
                    {"params", {{"name", "takefx.set_param"},
                                {"arguments", {{"track", 0}, {"item", 0}, {"fx", 0}, {"param", 1},
                                               {"normalized", 0.25}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].value("normalized", -1.0) == 0.25,
              "takefx.set_param echoes normalized (main-thread hop)");
    }
    {
        // send.set structured shape (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 835}, {"method", "tools/call"},
                    {"params", {{"name", "send.set"},
                                {"arguments", {{"track", 0}, {"index", 0}, {"db", -6.0}, {"mode", 1}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", false) && sc.value("index", -1) == 0, "send.set returns ok + index");
    }
    {
        // send.set_channels encodes I_SRCCHAN (mono at offset 4 -> (4)|(1<<10)=1028)
        json req = {{"jsonrpc", "2.0"}, {"id", 836}, {"method", "tools/call"},
                    {"params", {{"name", "send.set_channels"},
                                {"arguments", {{"track", 0}, {"index", 0}, {"srcChannel", 4},
                                               {"srcChannels", 1}, {"dstChannel", 2}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].value("srcChan", -1) == 1028,
              "send.set_channels encodes a mono send offset (4|1024=1028)");
    }
    {
        // receive.list read-only shape
        json req = {{"jsonrpc", "2.0"}, {"id", 837}, {"method", "tools/call"},
                    {"params", {{"name", "receive.list"}, {"arguments", {{"track", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("receiveCount") && sc["receives"].is_array(),
              "receive.list returns receiveCount + receives[]");
    }
    {
        // tempo.add_marker required-arg contract (missing bpm) + valid call shape
        json bad = {{"jsonrpc", "2.0"}, {"id", 838}, {"method", "tools/call"},
                    {"params", {{"name", "tempo.add_marker"}, {"arguments", {{"position", 2.0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "tempo.add_marker without 'bpm' -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 839}, {"method", "tools/call"},
                   {"params", {{"name", "tempo.add_marker"},
                               {"arguments", {{"position", 2.0}, {"bpm", 140.0}}}}}};
        check(rpc(cli, cfg.token, ok)["result"]["structuredContent"].value("ok", false),
              "tempo.add_marker(position,bpm) -> ok");
    }
    {
        // time conversions round-trip through the (host stub) tempo map
        json req = {{"jsonrpc", "2.0"}, {"id", 840}, {"method", "tools/call"},
                    {"params", {{"name", "time.qn_to_time"}, {"arguments", {{"qn", 4.0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"]["structuredContent"].contains("time"), "time.qn_to_time returns time");
    }
    {
        // project.render defaults to dryRun (never renders in the test); structured shape
        json req = {{"jsonrpc", "2.0"}, {"id", 841}, {"method", "tools/call"},
                    {"params", {{"name", "project.render"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", false) && sc.value("dryRun", false) == true,
              "project.render defaults to dryRun=true (no render)");
    }
    {
        // project.open requires 'path'
        json req = {{"jsonrpc", "2.0"}, {"id", 842}, {"method", "tools/call"},
                    {"params", {{"name", "project.open"}, {"arguments", json::object()}}}};
        check(rpc(cli, cfg.token, req)["result"].value("isError", false),
              "project.open without 'path' -> isError");
    }
    {
        // master.get read-only shape + master.set_fx_param required arg contract
        json req = {{"jsonrpc", "2.0"}, {"id", 843}, {"method", "tools/call"},
                    {"params", {{"name", "master.get"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("volDb") && sc.contains("fxCount"), "master.get returns volDb + fxCount");
        json bad = {{"jsonrpc", "2.0"}, {"id", 844}, {"method", "tools/call"},
                    {"params", {{"name", "master.set_fx_param"}, {"arguments", {{"fx", 0}, {"param", 0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "master.set_fx_param without value/normalized -> isError");
    }

    // --- 2f. Phase-8 (D3) track.set_state_chunk: unified diff + structural validation + gate ---
    {
        // (i) SDK-free structural validation
        check(validateTrackChunk("<TRACK\nNAME \"Drums\"\n>\n").empty(),
              "validateTrackChunk accepts a minimal balanced <TRACK ...> block");
        check(!validateTrackChunk("this is not a chunk").empty(),
              "validateTrackChunk rejects text not starting with <TRACK");
        check(!validateTrackChunk("<TRACK\nNAME x\n").empty(),
              "validateTrackChunk rejects an unclosed block");
        check(!validateTrackChunk("<TRACK\n>\n>\n").empty(),
              "validateTrackChunk rejects a stray closing '>'");

        // (ii) SDK-free unified diff
        const std::string base =
            "<TRACK\nNAME \"Drums\"\nVOLPAN 1 0\n<FXCHAIN\nBYPASS 0 0 0\n>\n>\n";
        ChunkDiff same = diffTrackChunks(base, base);
        check(!same.changed && same.text.empty(), "diffTrackChunks: identical -> unchanged, empty diff");

        const std::string edited =
            "<TRACK\nNAME \"Drums\"\nVOLPAN 0.5 0\n<FXCHAIN\nBYPASS 0 0 0\n>\n>\n";
        ChunkDiff d = diffTrackChunks(base, edited);
        check(d.changed && d.added == 1 && d.removed == 1, "diffTrackChunks: 1-line edit -> +1/-1");
        check(d.text.find("@@") != std::string::npos &&
                  d.text.find("-VOLPAN 1 0") != std::string::npos &&
                  d.text.find("+VOLPAN 0.5 0") != std::string::npos &&
                  d.text.find(" NAME \"Drums\"") != std::string::npos,
              "diffTrackChunks: unified hunk shows -/+ lines with surrounding context");

        ChunkDiff ins = diffTrackChunks("<TRACK\nA\n>\n", "<TRACK\nA\nB\n>\n");
        check(ins.changed && ins.added == 1 && ins.removed == 0, "diffTrackChunks: pure insert -> +1/-0");

        // (iii) tool presence (core profile)
        json listReq = {{"jsonrpc", "2.0"}, {"id", 850}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        bool present = false;
        for (const auto& t : list)
            if (t.value("name", "") == "track.set_state_chunk") present = true;
        check(present, "track.set_state_chunk present in tools/list (core profile)");

        // (iv) required-arg contract (thrown -> isError)
        json noChunk = {{"jsonrpc", "2.0"}, {"id", 851}, {"method", "tools/call"},
                        {"params", {{"name", "track.set_state_chunk"}, {"arguments", {{"track", 0}}}}}};
        check(rpc(cli, cfg.token, noChunk)["result"].value("isError", false),
              "track.set_state_chunk without 'chunk' -> isError");
        json noTrack = {{"jsonrpc", "2.0"}, {"id", 852}, {"method", "tools/call"},
                        {"params", {{"name", "track.set_state_chunk"},
                                    {"arguments", {{"chunk", "<TRACK\n>\n"}}}}}};
        check(rpc(cli, cfg.token, noTrack)["result"].value("isError", false),
              "track.set_state_chunk without 'track' -> isError");

        // (v) rail (a): invalid chunk surfaces as a structured error BEFORE any REAPER call
        json badChunk = {{"jsonrpc", "2.0"}, {"id", 853}, {"method", "tools/call"},
                         {"params", {{"name", "track.set_state_chunk"},
                                     {"arguments", {{"track", 0}, {"chunk", "this is not a chunk"}}}}}};
        json badResp = rpc(cli, cfg.token, badChunk);
        check(badResp["result"].value("isError", true) == false &&
                  badResp["result"]["structuredContent"].value("error", "") == "invalid_chunk",
              "track.set_state_chunk rejects a non-<TRACK chunk with error=invalid_chunk");

        // (vi) rail (b): dryRun preview returns ok + a non-empty diff (host baseline is empty)
        json dry = {{"jsonrpc", "2.0"}, {"id", 854}, {"method", "tools/call"},
                    {"params", {{"name", "track.set_state_chunk"},
                                {"arguments", {{"track", 0},
                                               {"chunk", "<TRACK\nNAME \"X\"\n>\n"},
                                               {"dryRun", true}}}}}};
        json dryResp = rpc(cli, cfg.token, dry);
        const json& dsc = dryResp["result"]["structuredContent"];
        check(dsc.value("ok", false) && dsc.value("changed", false) &&
                  dsc.value("addedLines", 0) >= 1 && !dsc.value("diff", std::string()).empty(),
              "track.set_state_chunk dryRun returns ok + non-empty diff (host preview)");
    }

    // --- 2f2. Phase-8 (post) track.patch_state: SDK-free key patch + tool contract/gate ---
    {
        // (i) SDK-free targeted top-level key patch
        ChunkPatchResult pr = applyTrackChunkKeyPatches(
            "<TRACK\nNAME \"Drums\"\nPEAKCOL 1\n>\n", {{"NAME", "\"Kick\""}});
        check(pr.error.empty() && pr.appliedKeys.size() == 1 &&
                  pr.chunk.find("NAME \"Kick\"") != std::string::npos &&
                  pr.chunk.find("NAME \"Drums\"") == std::string::npos,
              "applyTrackChunkKeyPatches replaces a top-level key value");

        // patching a key to its CURRENT value round-trips byte-identically (the no-op invariant)
        ChunkPatchResult noop = applyTrackChunkKeyPatches(
            "<TRACK\nNAME \"Drums\"\n>\n", {{"NAME", "\"Drums\""}});
        check(noop.error.empty() && noop.chunk == std::string("<TRACK\nNAME \"Drums\"\n>\n"),
              "applyTrackChunkKeyPatches: patch to current value -> byte-identical chunk (no-op)");

        // an absent top-level key fails closed
        ChunkPatchResult miss = applyTrackChunkKeyPatches(
            "<TRACK\nNAME \"Drums\"\n>\n", {{"PEAKCOL", "1"}});
        check(miss.error == "key_not_found" && miss.missingKeys.size() == 1,
              "applyTrackChunkKeyPatches: absent top-level key -> key_not_found");

        // a key that only exists INSIDE a nested block is not reachable as top-level
        ChunkPatchResult nested = applyTrackChunkKeyPatches(
            "<TRACK\nNAME \"Drums\"\n<FXCHAIN\nBYPASS 0 0 0\n>\n>\n", {{"BYPASS", "1 1 1"}});
        check(nested.error == "key_not_found",
              "applyTrackChunkKeyPatches: nested-block key is not a top-level key");

        // validateKeyPatches shape rules
        check(!validateKeyPatches({{"", "x"}}).empty(), "validateKeyPatches rejects an empty key");
        check(!validateKeyPatches({{"NAME", "a\nb"}}).empty(),
              "validateKeyPatches rejects a multi-line value");
        check(!validateKeyPatches({{"NAME", "x"}, {"NAME", "y"}}).empty(),
              "validateKeyPatches rejects duplicate keys");
        check(validateKeyPatches({{"NAME", "\"x\""}, {"PEAKCOL", "1"}}).empty(),
              "validateKeyPatches accepts distinct single-line patches");

        // small helper: an unambiguous single-patch array
        auto patchArr = [](const std::string& k, const std::string& v) {
            json arr = json::array();
            arr.push_back({{"key", k}, {"value", v}});
            return arr;
        };

        // (ii) tool presence (core profile)
        json listReq = {{"jsonrpc", "2.0"}, {"id", 8551}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        bool present = false;
        for (const auto& t : listResp["result"]["tools"])
            if (t.value("name", "") == "track.patch_state") present = true;
        check(present, "track.patch_state present in tools/list (core profile)");

        // (iii) required-arg contract (thrown -> isError)
        json noPatches = {{"jsonrpc", "2.0"}, {"id", 8552}, {"method", "tools/call"},
                          {"params", {{"name", "track.patch_state"}, {"arguments", {{"track", 0}}}}}};
        check(rpc(cli, cfg.token, noPatches)["result"].value("isError", false),
              "track.patch_state without 'patches' -> isError");
        json noTrack = {{"jsonrpc", "2.0"}, {"id", 8553}, {"method", "tools/call"},
                        {"params", {{"name", "track.patch_state"},
                                    {"arguments", {{"patches", patchArr("NAME", "\"X\"")}}}}}};
        check(rpc(cli, cfg.token, noTrack)["result"].value("isError", false),
              "track.patch_state without 'track' -> isError");

        // (iv) invalid_patch: a duplicate key is a structured error before any REAPER call
        json dupArr = json::array();
        dupArr.push_back({{"key", "NAME"}, {"value", "\"A\""}});
        dupArr.push_back({{"key", "NAME"}, {"value", "\"B\""}});
        json dup = {{"jsonrpc", "2.0"}, {"id", 8554}, {"method", "tools/call"},
                    {"params", {{"name", "track.patch_state"},
                                {"arguments", {{"track", 0}, {"patches", dupArr}}}}}};
        check(rpc(cli, cfg.token, dup)["result"]["structuredContent"].value("error", "") == "invalid_patch",
              "track.patch_state rejects duplicate keys with error=invalid_patch");

        // (v) key_not_found: a top-level key absent from the host baseline surfaces structured
        json miss2 = {{"jsonrpc", "2.0"}, {"id", 8555}, {"method", "tools/call"},
                      {"params", {{"name", "track.patch_state"},
                                  {"arguments", {{"track", 0}, {"patches", patchArr("NOTAKEY", "1")}}}}}};
        check(rpc(cli, cfg.token, miss2)["result"]["structuredContent"].value("error", "") == "key_not_found",
              "track.patch_state on an absent top-level key -> error=key_not_found");

        // (vi) dryRun preview: patch NAME on the host baseline -> ok + changed + non-empty diff + keys
        json dry = {{"jsonrpc", "2.0"}, {"id", 8556}, {"method", "tools/call"},
                    {"params", {{"name", "track.patch_state"},
                                {"arguments", {{"track", 0}, {"patches", patchArr("NAME", "\"Kick\"")},
                                               {"dryRun", true}}}}}};
        json dryResp = rpc(cli, cfg.token, dry)["result"];
        const json& dsc = dryResp["structuredContent"];
        check(dsc.value("ok", false) && dsc.value("changed", false) &&
                  !dsc.value("diff", std::string()).empty() &&
                  dsc["keys"].size() == 1 && dsc["keys"][0] == "NAME",
              "track.patch_state dryRun previews a NAME patch (ok+changed+diff+keys)");

        // (vii) no-op: patching NAME to its current baseline value changes nothing
        json same = {{"jsonrpc", "2.0"}, {"id", 8557}, {"method", "tools/call"},
                     {"params", {{"name", "track.patch_state"},
                                 {"arguments", {{"track", 0}, {"patches", patchArr("NAME", "\"Baseline\"")},
                                                {"dryRun", true}}}}}};
        check(rpc(cli, cfg.token, same)["result"]["structuredContent"].value("changed", true) == false,
              "track.patch_state: patching a key to its current value -> changed=false (no-op)");
    }

    // --- 2g. Phase-8 (P8-4) depth fill: envelope/MIDI depth, rec-mode, freeze, metering ---
    {
        // (i) tool presence
        json listReq = {{"jsonrpc", "2.0"}, {"id", 860}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        const char* p84[] = {
            "envelope.get_points", "envelope.delete_point", "envelope.clear_range",
            "envelope.set_automation_mode",
            "midi.set_note", "midi.set_cc", "midi.insert_notes", "midi.select_notes",
            "track.set_rec_mode", "track.freeze", "track.unfreeze", "track.get_peak",
            "master.remove_fx"};
        bool all = true; std::string missing;
        for (auto* n : p84) if (!listHas(n)) { all = false; missing = n; break; }
        check(all, all ? "all 13 P8-4 depth-fill tools present in tools/list"
                       : ("P8-4 tool missing from tools/list: " + missing));
    }
    {
        // (ii) envelope.set_automation_mode: required-arg + range contract, echo (host path)
        json bad = {{"jsonrpc", "2.0"}, {"id", 861}, {"method", "tools/call"},
                    {"params", {{"name", "envelope.set_automation_mode"},
                                {"arguments", {{"track", 0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "envelope.set_automation_mode without 'mode' -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 862}, {"method", "tools/call"},
                   {"params", {{"name", "envelope.set_automation_mode"},
                               {"arguments", {{"track", 0}, {"mode", 2}}}}}};
        json okResp = rpc(cli, cfg.token, ok);
        check(okResp["result"].value("isError", true) == false &&
                  okResp["result"]["structuredContent"].value("mode", -1) == 2,
              "envelope.set_automation_mode echoes mode=2 (touch)");
    }
    {
        // (iii) envelope.clear_range rejects end < start (SDK-independent guard)
        json bad = {{"jsonrpc", "2.0"}, {"id", 863}, {"method", "tools/call"},
                    {"params", {{"name", "envelope.clear_range"},
                                {"arguments", {{"track", 0}, {"envelope", "Volume"},
                                               {"start", 5.0}, {"end", 1.0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "envelope.clear_range with end<start -> isError");
    }
    {
        // (iv) midi.set_note required-arg contract ('index')
        json bad = {{"jsonrpc", "2.0"}, {"id", 864}, {"method", "tools/call"},
                    {"params", {{"name", "midi.set_note"},
                                {"arguments", {{"track", 0}, {"item", 0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "midi.set_note without 'index' -> isError");
    }
    {
        // (v) midi.insert_notes: array contract (rejects a note missing pitch; accepts a valid batch)
        json bad = {{"jsonrpc", "2.0"}, {"id", 865}, {"method", "tools/call"},
                    {"params", {{"name", "midi.insert_notes"},
                                {"arguments", {{"track", 0}, {"item", 0},
                                               {"notes", json::array({{{"startBeats", 0.0}}})}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "midi.insert_notes with a pitch-less note -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 866}, {"method", "tools/call"},
                   {"params", {{"name", "midi.insert_notes"},
                               {"arguments", {{"track", 0}, {"item", 0},
                                              {"notes", json::array({
                                                  {{"startBeats", 0.0}, {"pitch", 60}},
                                                  {{"startBeats", 1.0}, {"pitch", 64}},
                                                  {{"startBeats", 2.0}, {"pitch", 67}}})}}}}}};
        json okResp = rpc(cli, cfg.token, ok);
        check(okResp["result"].value("isError", true) == false &&
                  okResp["result"]["structuredContent"].value("inserted", 0) == 3,
              "midi.insert_notes inserts a 3-note batch (host echo)");
    }
    {
        // (vi) midi.select_notes echoes the selection flag (host path)
        json req = {{"jsonrpc", "2.0"}, {"id", 867}, {"method", "tools/call"},
                    {"params", {{"name", "midi.select_notes"},
                                {"arguments", {{"track", 0}, {"item", 0}, {"selected", false}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", true) == false &&
                  resp["result"]["structuredContent"].value("selected", true) == false,
              "midi.select_notes echoes selected=false");
    }
    {
        // (vii) track.set_rec_mode required-arg contract + echo
        json bad = {{"jsonrpc", "2.0"}, {"id", 868}, {"method", "tools/call"},
                    {"params", {{"name", "track.set_rec_mode"}, {"arguments", {{"track", 0}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "track.set_rec_mode without 'mode' -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 869}, {"method", "tools/call"},
                   {"params", {{"name", "track.set_rec_mode"},
                               {"arguments", {{"track", 0}, {"mode", 2}}}}}};
        check(rpc(cli, cfg.token, ok)["result"]["structuredContent"].value("mode", -1) == 2,
              "track.set_rec_mode echoes mode=2 (none/monitor-only)");
    }
    {
        // (viii) track.freeze validates its mode enum; host path returns ok + mode echo
        json bad = {{"jsonrpc", "2.0"}, {"id", 870}, {"method", "tools/call"},
                    {"params", {{"name", "track.freeze"},
                                {"arguments", {{"track", 0}, {"mode", "surround"}}}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "track.freeze with an unknown mode -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 871}, {"method", "tools/call"},
                   {"params", {{"name", "track.freeze"},
                               {"arguments", {{"track", 0}, {"mode", "multichannel"}}}}}};
        check(rpc(cli, cfg.token, ok)["result"]["structuredContent"].value("mode", "") ==
                  "multichannel",
              "track.freeze echoes mode=multichannel (host path)");
    }
    {
        // (ix) track.get_peak read-only shape
        json req = {{"jsonrpc", "2.0"}, {"id", 872}, {"method", "tools/call"},
                    {"params", {{"name", "track.get_peak"}, {"arguments", {{"track", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("channels") && sc["peaks"].is_array() && !sc["peaks"].empty() &&
                  sc["peaks"][0].contains("peakDb"),
              "track.get_peak returns channels + peaks[] with peakDb");
    }
    {
        // (x) master.remove_fx required-arg contract
        json bad = {{"jsonrpc", "2.0"}, {"id", 873}, {"method", "tools/call"},
                    {"params", {{"name", "master.remove_fx"}, {"arguments", json::object()}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false),
              "master.remove_fx without 'fx' -> isError");
    }
    {
        // (xi) track.get now reports recMode + automationMode (P8-4 consolidated-read additions)
        json req = {{"jsonrpc", "2.0"}, {"id", 874}, {"method", "tools/call"},
                    {"params", {{"name", "track.get"}, {"arguments", {{"track", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("recMode") && sc.contains("automationMode"),
              "track.get includes recMode + automationMode");
    }

    // --- 2h. Batch S1: spatial / ambisonic depth (convert_order/format, get_scene_info, mirror, beamform, room) ---
    {
        // (i) tool presence
        json listReq = {{"jsonrpc", "2.0"}, {"id", 880}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        const char* s1[] = {"spatial.convert_order", "spatial.convert_format", "spatial.get_scene_info",
                            "spatial.mirror_scene", "spatial.beamform", "spatial.set_distance"};
        bool all = true; std::string missing;
        for (auto* n : s1) if (!listHas(n)) { all = false; missing = n; break; }
        check(all, all ? "all 6 Batch-S1 spatial-depth tools present in tools/list"
                       : ("S1 tool missing from tools/list: " + missing));
    }
    {
        // (ii) convert_order required-arg contract ('toOrder')
        json bad = {{"jsonrpc", "2.0"}, {"id", 881}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.convert_order"}, {"arguments", {{"track", 0}}}}}};
        json badR = rpc(cli, cfg.token, bad);
        check(badR["result"].value("isError", false),
              "spatial.convert_order without 'toOrder' -> isError");
        // (iii) channel math: order 2 -> 9 ACN channels, even-padded to a 10-ch track
        json ok = {{"jsonrpc", "2.0"}, {"id", 882}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.convert_order"},
                               {"arguments", {{"track", 0}, {"toOrder", 2}}}}}};
        json okR = rpc(cli, cfg.token, ok);
        const json& sc = okR["result"]["structuredContent"];
        check(sc.value("hoaChannels", 0) == 9 && sc.value("channels", 0) == 10 &&
                  sc.value("padded", false) == true,
              "spatial.convert_order order2 -> hoaChannels 9, 10-ch padded bus");
        // (iv) order 1 -> 4 ACN channels, not padded
        json ok1 = {{"jsonrpc", "2.0"}, {"id", 883}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.convert_order"},
                                {"arguments", {{"track", 0}, {"toOrder", 1}}}}}};
        json ok1R = rpc(cli, cfg.token, ok1);
        const json& sc1 = ok1R["result"]["structuredContent"];
        check(sc1.value("hoaChannels", 0) == 4 && sc1.value("channels", 0) == 4 &&
                  sc1.value("padded", true) == false,
              "spatial.convert_order order1 -> hoaChannels 4, 4-ch bus (unpadded)");
    }
    {
        // (v) convert_format required args + no-op + unsupported + a valid mode
        json bad = {{"jsonrpc", "2.0"}, {"id", 884}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.convert_format"}, {"arguments", {{"track", 0}}}}}};
        json badR = rpc(cli, cfg.token, bad);
        check(badR["result"].value("isError", false),
              "spatial.convert_format without from/to -> isError");
        json noop = {{"jsonrpc", "2.0"}, {"id", 885}, {"method", "tools/call"},
                     {"params", {{"name", "spatial.convert_format"},
                                 {"arguments", {{"track", 0}, {"from", "SN3D"}, {"to", "ambiX"}}}}}};
        json noopR = rpc(cli, cfg.token, noop);
        const json& scn = noopR["result"]["structuredContent"];
        check(scn.value("noop", false) == true,
              "spatial.convert_format SN3D->ambiX is a no-op (ambiX == SN3D)");
        json uns = {{"jsonrpc", "2.0"}, {"id", 886}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.convert_format"},
                                {"arguments", {{"track", 0}, {"from", "N3D"}, {"to", "FuMa"}}}}}};
        json unsR = rpc(cli, cfg.token, uns);
        check(unsR["result"].value("isError", false),
              "spatial.convert_format N3D->FuMa (unsupported direct) -> isError");
        json okc = {{"jsonrpc", "2.0"}, {"id", 887}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.convert_format"},
                                {"arguments", {{"track", 0}, {"from", "SN3D"}, {"to", "N3D"}}}}}};
        json okcR = rpc(cli, cfg.token, okc);
        const json& scc = okcR["result"]["structuredContent"];
        check(scc.value("mode", -1) == 0 && scc.value("noop", true) == false,
              "spatial.convert_format SN3D->N3D resolves to mode 0 (per-degree gain)");
    }
    {
        // (vi) get_scene_info read-only shape
        json req = {{"jsonrpc", "2.0"}, {"id", 888}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.get_scene_info"}, {"arguments", {{"track", 0}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("channels") && sc.contains("inferredOrder") && sc["spatialFx"].is_array(),
              "spatial.get_scene_info returns channels + inferredOrder + spatialFx[]");
    }
    {
        // (vii) mirror_scene plane -> plane index mapping (front-back -> 1)
        json req = {{"jsonrpc", "2.0"}, {"id", 889}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.mirror_scene"},
                                {"arguments", {{"track", 0}, {"plane", "front-back"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("plane", "") == "front-back" && sc.value("planeIndex", -1) == 1,
              "spatial.mirror_scene front-back -> planeIndex 1");
    }
    {
        // (viii) beamform + set_distance host shape (plug-in-dependent live, but structured off-REAPER)
        json bf = {{"jsonrpc", "2.0"}, {"id", 890}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.beamform"},
                               {"arguments", {{"track", 0}, {"azimuthDeg", 45.0}}}}}};
        json bfR = rpc(cli, cfg.token, bf);
        const json& scb = bfR["result"]["structuredContent"];
        check(scb.value("ok", false) == true && scb.contains("beamformer"),
              "spatial.beamform returns ok + beamformer");
        json rm = {{"jsonrpc", "2.0"}, {"id", 891}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.set_distance"},
                               {"arguments", {{"track", 0}, {"distanceM", 3.0}}}}}};
        json rmR = rpc(cli, cfg.token, rm);
        const json& scr = rmR["result"]["structuredContent"];
        check(scr.value("ok", false) == true && scr.contains("roomEncoder"),
              "spatial.set_distance returns ok + roomEncoder");
    }

    // --- 2i. Batch M1: immersive metering suite (analysis.meter + analysis.spatial_field) ----------
    // Host build cannot render, so these exercise dispatch + tool presence + the host-path shape; the
    // measurement DSP itself (per-channel, correlation, DirAC DoA/diffuseness, rE/rV) is proven
    // deterministically in unit.meter against synthetic buffers.
    {
        // (i) both tools present in tools/list, under the Analysis profile.
        json listReq = {{"jsonrpc", "2.0"}, {"id", 900}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("analysis.meter") && listHas("analysis.spatial_field"),
              "both Batch-M1 metering tools present in tools/list");
    }
    {
        // (ii) analysis.meter dispatches and returns the host-path shape (target + measuredSource).
        json req = {{"jsonrpc", "2.0"}, {"id", 901}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.meter"},
                                {"arguments", {{"target", "master"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false, "analysis.meter dispatches without error");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("target", "") == "master" && sc.contains("measuredSource"),
              "analysis.meter returns target + measuredSource");
    }
    {
        // (iii) analysis.spatial_field dispatches and echoes the normalization arg.
        json req = {{"jsonrpc", "2.0"}, {"id", 902}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.spatial_field"},
                                {"arguments", {{"target", 0}, {"normalization", "n3d"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.spatial_field dispatches without error");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("normalization", "") == "n3d" && sc.contains("measuredSource"),
              "analysis.spatial_field echoes normalization + returns measuredSource");
    }

    // --- 2j. Batch M2: metering depth (stem/dialog/downmix/timeline + field trajectory) -----------
    // Host build cannot render; these prove presence, dispatch, required-arg contracts, and the
    // host-path shapes. The gating/windowing/downmix DSP is proven in unit.meter.
    {
        // (i) all four M2 tools present in tools/list.
        json listReq = {{"jsonrpc", "2.0"}, {"id", 910}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("analysis.stem_loudness") && listHas("analysis.dialog_loudness") &&
              listHas("analysis.downmix_check") && listHas("analysis.loudness_timeline"),
              "all four Batch-M2 metering tools present in tools/list");
    }
    {
        // (ii) stem_loudness: required-arg contract (tracks) + host-path shape.
        json bad = {{"jsonrpc", "2.0"}, {"id", 911}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.stem_loudness"},
                                {"arguments", json::object()}}}};
        json badResp = rpc(cli, cfg.token, bad);
        check(badResp["result"].value("isError", false) == true,
              "analysis.stem_loudness without tracks -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 912}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.stem_loudness"},
                               {"arguments", {{"tracks", {0, 1}}}}}}};
        json okResp = rpc(cli, cfg.token, ok);
        check(okResp["result"].value("isError", false) == false,
              "analysis.stem_loudness dispatches without error");
        const json& sc2j = okResp["result"]["structuredContent"];
        check(sc2j.contains("stems") && sc2j.contains("measuredSource"),
              "analysis.stem_loudness returns stems + measuredSource");
    }
    {
        // (iii) dialog_loudness: required-arg contract (dialogTrack) + host-path shape.
        json bad = {{"jsonrpc", "2.0"}, {"id", 913}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.dialog_loudness"},
                                {"arguments", json::object()}}}};
        json badResp = rpc(cli, cfg.token, bad);
        check(badResp["result"].value("isError", false) == true,
              "analysis.dialog_loudness without dialogTrack -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 914}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.dialog_loudness"},
                               {"arguments", {{"dialogTrack", 0}}}}}};
        json okResp = rpc(cli, cfg.token, ok);
        check(okResp["result"].value("isError", false) == false,
              "analysis.dialog_loudness dispatches without error");
        const json& sc2j = okResp["result"]["structuredContent"];
        check(sc2j.contains("program") && sc2j.contains("dialog") &&
              sc2j.contains("programToDialogLu"),
              "analysis.dialog_loudness returns program + dialog + offset fields");
    }
    {
        // (iv) downmix_check dispatches (host path) and echoes the target.
        json req = {{"jsonrpc", "2.0"}, {"id", 915}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.downmix_check"},
                                {"arguments", {{"target", "master"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.downmix_check dispatches without error");
        const json& sc2j = resp["result"]["structuredContent"];
        check(sc2j.value("target", "") == "master" && sc2j.contains("measuredSource"),
              "analysis.downmix_check returns target + measuredSource");
    }
    {
        // (v) loudness_timeline dispatches (host path) with the series fields present.
        json req = {{"jsonrpc", "2.0"}, {"id", 916}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.loudness_timeline"},
                                {"arguments", {{"hopSec", 0.5}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.loudness_timeline dispatches without error");
        const json& sc2j = resp["result"]["structuredContent"];
        check(sc2j.contains("times") && sc2j.contains("momentary") && sc2j.contains("shortTerm"),
              "analysis.loudness_timeline returns the series fields");
    }
    {
        // (vi) spatial_field accepts the M2 timeline args (host path).
        json req = {{"jsonrpc", "2.0"}, {"id", 917}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.spatial_field"},
                                {"arguments", {{"target", 0}, {"timeline", true},
                                               {"windowSec", 0.5}, {"hopSec", 0.25}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.spatial_field accepts timeline/windowSec/hopSec args");
    }

    // --- 2k. Batch M3: metering depth (object loudness / binaural check / decode coverage) ---------
    // Host build cannot render; these prove presence, dispatch, required-arg contracts, and the
    // host-path shapes. The gated-loudness + SN3D decode/coverage DSP is proven in unit.meter.
    {
        // (i) all three M3 tools present in tools/list.
        json listReq = {{"jsonrpc", "2.0"}, {"id", 920}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("analysis.object_loudness") && listHas("analysis.binaural_check") &&
              listHas("analysis.decode_coverage"),
              "all three Batch-M3 metering tools present in tools/list");
    }
    {
        // (ii) object_loudness: required-arg contract (objects) + host-path shape.
        json bad = {{"jsonrpc", "2.0"}, {"id", 921}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.object_loudness"},
                                {"arguments", json::object()}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false) == true,
              "analysis.object_loudness without objects -> isError");
        json ok = {{"jsonrpc", "2.0"}, {"id", 922}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.object_loudness"},
                               {"arguments", {{"objects", {0, 1}}}}}}};
        json okResp = rpc(cli, cfg.token, ok);
        check(okResp["result"].value("isError", false) == false,
              "analysis.object_loudness dispatches without error");
        const json& sc = okResp["result"]["structuredContent"];
        check(sc.contains("objects") && sc.contains("measuredSource"),
              "analysis.object_loudness returns objects + measuredSource");
    }
    {
        // (iii) binaural_check dispatches (host path) and echoes the target.
        json req = {{"jsonrpc", "2.0"}, {"id", 923}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.binaural_check"},
                                {"arguments", {{"target", "master"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.binaural_check dispatches without error");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("target", "") == "master" && sc.contains("measuredSource"),
              "analysis.binaural_check returns target + measuredSource");
    }
    {
        // (iv) decode_coverage: default uniform-sphere mode + speaker-count echo (host path).
        json req = {{"jsonrpc", "2.0"}, {"id", 924}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.decode_coverage"},
                                {"arguments", {{"target", 0}, {"speakers", 96}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.decode_coverage dispatches without error");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("mode", "") == "uniform-sphere" && sc.value("speakers", 0) == 96 &&
              sc.contains("measuredSource"),
              "analysis.decode_coverage echoes uniform-sphere mode + speaker count");
    }
    {
        // (v) decode_coverage named-layout mode builds the layout's directional speaker set (host
        //     path): 7.1.4 => 11 directional speakers (LFE omitted), mode = 'layout:7.1.4'.
        json req = {{"jsonrpc", "2.0"}, {"id", 925}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.decode_coverage"},
                                {"arguments", {{"target", 0}, {"layout", "7.1.4"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "analysis.decode_coverage accepts a named layout");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("mode", "") == "layout:7.1.4" && sc.value("speakers", 0) == 11,
              "analysis.decode_coverage builds 11 directional speakers for 7.1.4");
    }

    // --- 3. tools/call read-only (transport.get_state) ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                    {"params", {{"name", "transport.get_state"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp.contains("result"), "get_state call has result");
        const json& r = resp["result"];
        check(r.value("isError", true) == false, "get_state not an error");
        check(r["content"].is_array() && !r["content"].empty(), "get_state returns content[]");
        check(r["content"][0].value("type", "") == "text", "content[0] is text");
        const json& sc = r["structuredContent"];
        check(sc.contains("playState") && sc.contains("tempo") && sc.contains("loop"),
              "get_state structuredContent has required fields");
    }

    // --- 4. tools/call mutating (track.add) round-trips arguments ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
                    {"params", {{"name", "track.add"}, {"arguments", {{"index", 2}, {"name", "Foo"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("trackIndex", -1) == 2, "track.add echoes requested index (main-thread hop)");
        check(sc.contains("guid"), "track.add returns a guid field");
    }

    // --- 5. tools/call missing required arg -> tool error (not a crash) ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                    {"params", {{"name", "track.get_name"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false), "missing-arg call surfaces isError=true");
    }

    // --- 6. unknown tool -> JSON-RPC error ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"},
                    {"params", {{"name", "does.not.exist"}, {"arguments", json::object()}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp.contains("error") && resp["error"].value("code", 0) == -32602,
              "unknown tool -> JSON-RPC error -32602");
    }

    // --- 6b. Phase-5e session.run_dsl: dryRun plan, real apply, validation error, parse error ---
    {
        // dryRun: two dry-run-previewable verbs -> composed plan + diff, nothing mutates.
        const std::string script =
            "# master + conformance check\n"
            "apply_style track=0 style=pop-bright\n"
            "check spec=ebu-r128 measured={\"lufsIntegrated\":-23,\"truePeak\":-1,\"channels\":2}";
        json req = {{"jsonrpc", "2.0"}, {"id", 60}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"},
                                {"arguments", {{"script", script}, {"dryRun", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(resp["result"].value("isError", true) == false, "run_dsl dryRun is not a transport error");
        check(sc.value("ok", false) && sc.value("dryRun", false), "run_dsl dryRun -> ok + dryRun");
        check(sc.value("stepCount", 0) == 2, "run_dsl parsed 2 statements");
        check(sc["steps"].is_array() && sc["steps"].size() == 2, "run_dsl previewed both steps");
        check(sc["steps"][0].value("tool", "") == "mix.apply_style" &&
              sc["steps"][1].value("tool", "") == "analysis.check_deliverable",
              "run_dsl resolved verbs to the right tools, in order");
        check(sc["steps"][0].value("error", true) == false, "step 1 (apply_style dryRun) not an error");
        check(sc.contains("plan") && sc["plan"].is_array(), "run_dsl dryRun carries the structured plan");
        check(sc.value("diff", "").find("mix.apply_style") != std::string::npos, "run_dsl diff names the tool");
    }
    {
        // apply: same host-safe script executed for real -> composed results, one undo point.
        const std::string script =
            "apply_style track=0 style=pop-bright\n"
            "check spec=ebu-r128 measured={\"lufsIntegrated\":-23,\"truePeak\":-1,\"channels\":2}";
        json req = {{"jsonrpc", "2.0"}, {"id", 61}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"}, {"arguments", {{"script", script}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", false) == true, "run_dsl apply -> ok");
        check(sc.value("stepCount", 0) == 2 && sc["steps"].size() == 2, "run_dsl apply ran both steps");
        check(sc["steps"][1]["result"].contains("checks"), "run_dsl composed the check_deliverable result");
        check(sc.value("undo", "").find("undo point") != std::string::npos, "run_dsl reports a single undo point");
    }
    {
        // validation pre-flight: a bad style is caught with zero mutation before the real apply.
        json req = {{"jsonrpc", "2.0"}, {"id", 62}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"},
                                {"arguments", {{"script", "apply_style track=0 style=does-not-exist"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_validation_error",
              "run_dsl apply pre-flights and rejects a bad style before mutating");
        check(sc.value("failedStep", 0) == 1, "validation error points at the offending step");
    }
    {
        // parse error: a syntactically broken script is rejected structurally, no dispatch.
        json req = {{"jsonrpc", "2.0"}, {"id", 63}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"},
                                {"arguments", {{"script", "frobnicate x=1"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_parse_error",
              "run_dsl rejects an unknown verb as a parse error");
        check(sc["errors"].is_array() && sc["errors"][0].value("code", "") == "unknown_verb",
              "parse error carries the structured code");
    }

    // --- 6c. Phase-6(a): $ref / capture bindings in session.run_dsl ---
    {
        // Capture build_bed's result, then feed its busTrack into apply_style -> end-to-end resolve.
        const std::string script =
            "$bed = build_bed layout=7.1.4 index=5\n"
            "apply_style track=$bed.busTrack style=pop-bright";
        json req = {{"jsonrpc", "2.0"}, {"id", 64}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"}, {"arguments", {{"script", script}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", false) == true, "run_dsl $ref apply -> ok");
        check(sc.value("stepCount", 0) == 2 && sc["steps"].size() == 2, "capture + consumer both ran");
        check(sc["steps"][1]["result"].value("track", -1) == 5,
              "$bed.busTrack resolved to the captured bus index (5) and reached apply_style");
        check(sc.value("undo", "").find("undo point") != std::string::npos,
              "the $ref script is still ONE undo point");
    }
    {
        // dryRun: the ref-bearing step is deferred (not concretely previewed); the diff shows the $ref.
        const std::string script =
            "$bed = build_bed layout=7.1.4 index=5\n"
            "apply_style track=$bed.busTrack style=pop-bright";
        json req = {{"jsonrpc", "2.0"}, {"id", 65}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"},
                                {"arguments", {{"script", script}, {"dryRun", true}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", false) && sc.value("dryRun", false), "run_dsl $ref dryRun -> ok, mutates nothing");
        check(sc["steps"][1]["result"].value("previewed", true) == false,
              "the ref-bearing step is deferred in dryRun (not concretely previewed)");
        check(sc.value("diff", "").find("$bed.busTrack") != std::string::npos,
              "dryRun diff renders the $ref placeholder readably");
    }
    {
        // A $ref to a field the producer never returned -> apply-time dsl_ref_unresolved + atomic rollback.
        const std::string script =
            "$bed = build_bed layout=7.1.4 index=5\n"
            "apply_style track=$bed.nonexistent style=pop-bright";
        json req = {{"jsonrpc", "2.0"}, {"id", 66}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"}, {"arguments", {{"script", script}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_step_failed",
              "a missing captured field is a step failure");
        check(sc.value("failedStep", 0) == 2, "the failure points at the consumer step");
        check(sc["stepError"].value("error", "") == "dsl_ref_unresolved",
              "the step error is dsl_ref_unresolved");
        check(sc.value("rolledBack", false) == true, "atomic rollback fired (the captured step had applied)");
    }
    {
        // Static declared-before-use: a forward ref is rejected structurally, nothing dispatched.
        const std::string script =
            "apply_style track=$bed.busTrack style=pop-bright\n"
            "$bed = build_bed layout=7.1.4";
        json req = {{"jsonrpc", "2.0"}, {"id", 67}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"}, {"arguments", {{"script", script}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_ref_undeclared",
              "a forward $ref is rejected as dsl_ref_undeclared before any mutation");
        check(sc.value("line", 0) == 1, "the static error points at the offending line");
    }
    {
        // Duplicate capture name -> dsl_dup_capture, structurally rejected.
        const std::string script =
            "$bed = build_bed layout=7.1.4\n"
            "$bed = build_bed layout=5.1";
        json req = {{"jsonrpc", "2.0"}, {"id", 68}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"}, {"arguments", {{"script", script}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_dup_capture",
              "a duplicate capture name -> dsl_dup_capture");
    }
    {
        // A malformed capture form is a parse error (rejected before any dispatch).
        json req = {{"jsonrpc", "2.0"}, {"id", 69}, {"method", "tools/call"},
                    {"params", {{"name", "session.run_dsl"},
                                {"arguments", {{"script", "$bed=build_bed layout=7.1.4"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("ok", true) == false && sc.value("error", "") == "dsl_parse_error",
              "the no-space capture form is a parse error");
        check(sc["errors"][0].value("code", "") == "bad_capture", "parse error carries the bad_capture code");
    }

    // --- 7. ping ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}};
        json resp = rpc(cli, cfg.token, req);
        check(resp.contains("result"), "ping has result");
    }

    // --- 8. auth rejection (no/wrong bearer token) ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "ping"}};
        auto res = cli.Post("/mcp", req.dump(), "application/json");  // no Authorization header
        check(res && res->status == 401, "missing bearer token -> HTTP 401");
    }

    // --- 9. tools.enumerate meta-tool: sees the full surface, across all profiles ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"},
                    {"params", {{"name", "tools.enumerate"}, {"arguments", {{"profile", "all"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("count", 0) == sc.value("totalRegistered", -1),
              "tools.enumerate(all) count == totalRegistered");
        bool hasMarker = false, hasSpatial = false;
        for (const auto& t : sc["tools"]) {
            const std::string n = t.value("name", "");
            if (n == "marker.add") hasMarker = true;
            if (n == "spatial.build_bed") hasSpatial = true;
        }
        check(hasMarker && hasSpatial, "tools.enumerate lists tools across profiles");
    }

    // --- 9b. tools.enumerate profile + query filters ---
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 91}, {"method", "tools/call"},
                    {"params", {{"name", "tools.enumerate"}, {"arguments", {{"profile", "spatial"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        bool onlySpatial = !sc["tools"].empty();
        for (const auto& t : sc["tools"])
            if (t.value("profile", "") != "spatial") onlySpatial = false;
        check(onlySpatial, "tools.enumerate(profile=spatial) returns only spatial-profile tools");

        json q = {{"jsonrpc", "2.0"}, {"id", 92}, {"method", "tools/call"},
                  {"params", {{"name", "tools.enumerate"}, {"arguments", {{"query", "tempo"}}}}}};
        json qr = rpc(cli, cfg.token, q);
        bool sawTempoGet = false;
        for (const auto& t : qr["result"]["structuredContent"]["tools"])
            if (t.value("name", "") == "tempo.get") sawTempoGet = true;
        check(sawTempoGet, "tools.enumerate(query=tempo) finds tempo.get");
    }

    // --- 10. representative Phase-2 calls (host/fallback path echoes args) ---
    {
        json m = {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/call"},
                  {"params", {{"name", "marker.add"}, {"arguments", {{"position", 12.5}, {"name", "cue"}}}}}};
        json mr = rpc(cli, cfg.token, m);
        check(mr["result"].value("isError", true) == false, "marker.add succeeds");
        check(mr["result"]["structuredContent"].value("position", 0.0) == 12.5,
              "marker.add echoes position");

        json tp = {{"jsonrpc", "2.0"}, {"id", 101}, {"method", "tools/call"},
                   {"params", {{"name", "tempo.get"}, {"arguments", json::object()}}}};
        json tr = rpc(cli, cfg.token, tp);
        check(tr["result"]["structuredContent"].contains("bpm"), "tempo.get returns bpm");

        json sc = {{"jsonrpc", "2.0"}, {"id", 102}, {"method", "tools/call"},
                   {"params", {{"name", "track.set_channels"}, {"arguments", {{"track", 0}, {"channels", 13}}}}}};
        json scr = rpc(cli, cfg.token, sc);
        check(scr["result"]["structuredContent"].value("channels", 0) == 14,
              "track.set_channels rounds an odd channel count up to even (13 -> 14)");
    }

    // --- 11. profile negotiation: narrow to spatial; meta-tool stays visible; a core tool is
    //         hidden from tools/list but STILL callable by name (the lazy-surface guarantee) ---
    {
        json init = {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "initialize"},
                     {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
                                 {"profiles", {"spatial"}}}}};
        rpc(cli, cfg.token, init);

        json lreq = {{"jsonrpc", "2.0"}, {"id", 111}, {"method", "tools/list"}, {"params", json::object()}};
        json lr = rpc(cli, cfg.token, lreq);
        bool sawSpatial = false, sawMeta = false, sawMarker = false;
        for (const auto& t : lr["result"]["tools"]) {
            const std::string n = t.value("name", "");
            if (n == "spatial.build_bed") sawSpatial = true;
            if (n == "tools.enumerate") sawMeta = true;
            if (n == "marker.add") sawMarker = true;
        }
        check(sawSpatial, "narrowed(spatial): spatial tools listed");
        check(sawMeta, "narrowed(spatial): always-on tools.enumerate still listed");
        check(!sawMarker, "narrowed(spatial): core marker.add NOT in tools/list");

        json call = {{"jsonrpc", "2.0"}, {"id", 112}, {"method", "tools/call"},
                     {"params", {{"name", "marker.add"}, {"arguments", {{"position", 1.0}}}}}};
        json cr = rpc(cli, cfg.token, call);
        check(cr["result"].value("isError", true) == false,
              "narrowed(spatial): un-listed marker.add still callable (lazy surface)");
    }

    // --- 12. discovery file: write {url,token,sessionId,pid}, read back, remove ---
    {
        const std::string path = "reaper_mcp_discovery_test.json";
        json info = {{"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"},
                     {"token", cfg.token}, {"sessionId", server.sessionId()}, {"pid", 4321}};
        check(writeDiscoveryFile(path, info), "writeDiscoveryFile succeeds");
        {
            std::ifstream is(path);
            check(is.good(), "discovery file exists on disk");
            json readback;
            is >> readback;
            check(readback.value("token", "") == cfg.token, "discovery file round-trips token");
            check(readback.value("sessionId", "") == server.sessionId(),
                  "discovery file round-trips sessionId");
            check(readback.value("pid", 0) == 4321, "discovery file round-trips pid");
        }
        removeDiscoveryFile(path);
        std::ifstream is2(path);
        check(!is2.good(), "removeDiscoveryFile deletes it");
    }

    // --- 13. Phase-2b MIDI/take tools (host/fallback path echoes args, exercises the contract) ---
    {
        // re-init to the full surface (block 11 narrowed to spatial)
        json init = {{"jsonrpc", "2.0"}, {"id", 130}, {"method", "initialize"},
                     {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}}}};
        rpc(cli, cfg.token, init);

        json ci = {{"jsonrpc", "2.0"}, {"id", 131}, {"method", "tools/call"},
                   {"params", {{"name", "midi.create_item"},
                               {"arguments", {{"track", 0}, {"position", 0}, {"length", 4}}}}}};
        json cir = rpc(cli, cfg.token, ci);
        check(cir["result"].value("isError", true) == false, "midi.create_item succeeds");
        check(cir["result"]["structuredContent"].contains("item"), "midi.create_item returns item index");

        json in = {{"jsonrpc", "2.0"}, {"id", 132}, {"method", "tools/call"},
                   {"params", {{"name", "midi.insert_note"},
                               {"arguments", {{"track", 0}, {"item", 0}, {"startBeats", 0.0},
                                              {"lengthBeats", 1.0}, {"pitch", 60}, {"velocity", 100}}}}}};
        json inr = rpc(cli, cfg.token, in);
        const json& insc = inr["result"]["structuredContent"];
        check(inr["result"].value("isError", true) == false, "midi.insert_note succeeds");
        check(insc.value("pitch", -1) == 60, "midi.insert_note echoes pitch");
        check(insc.value("velocity", -1) == 100, "midi.insert_note echoes velocity");

        json ln = {{"jsonrpc", "2.0"}, {"id", 133}, {"method", "tools/call"},
                   {"params", {{"name", "midi.list_notes"}, {"arguments", {{"track", 0}, {"item", 0}}}}}};
        json lnr = rpc(cli, cfg.token, ln);
        check(lnr["result"]["structuredContent"].contains("noteCount"), "midi.list_notes returns noteCount");
        check(lnr["result"]["structuredContent"]["notes"].is_array(), "midi.list_notes returns notes[]");

        json cc = {{"jsonrpc", "2.0"}, {"id", 134}, {"method", "tools/call"},
                   {"params", {{"name", "midi.insert_cc"},
                               {"arguments", {{"track", 0}, {"item", 0}, {"beats", 1.0},
                                              {"messageType", "cc"}, {"cc", 7}, {"value", 100}}}}}};
        json ccr = rpc(cli, cfg.token, cc);
        check(ccr["result"].value("isError", true) == false, "midi.insert_cc succeeds");
        check(ccr["result"]["structuredContent"].value("messageType", "") == "cc",
              "midi.insert_cc echoes messageType");

        json tl = {{"jsonrpc", "2.0"}, {"id", 135}, {"method", "tools/call"},
                   {"params", {{"name", "take.list"}, {"arguments", {{"track", 0}, {"item", 0}}}}}};
        json tlr = rpc(cli, cfg.token, tl);
        check(tlr["result"]["structuredContent"].contains("takeCount"), "take.list returns takeCount");
    }

    // --- 13c. Phase-4 batch-d render deliverables (host path: plan shape, format id, Atmos layout) ---
    {
        json rd = {{"jsonrpc", "2.0"}, {"id", 136}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.render_deliverables"},
                               {"arguments", {{"targets", json::array({"multichannel", "ambix",
                                                                        "binaural", "atmos-send-layout"})},
                                              {"dryRun", true}, {"bedLayout", "7.1.4"},
                                              {"objectTracks", json::array({5, 6, 7})}}}}}};
        json rdr = rpc(cli, cfg.token, rd);
        check(rdr["result"].value("isError", true) == false, "render_deliverables succeeds (host)");
        const json& sc = rdr["result"]["structuredContent"];
        check(sc["rendered"].is_array() && sc["rendered"].size() == 3,
              "render_deliverables plans the 3 in-box masters (atmos handled separately)");
        check(sc.value("format", "") == "evaw", "render_deliverables defaults to WAV (evaw) format id");
        bool binaural2 = false, ambiOrder = false;
        for (const auto& r : sc["rendered"]) {
            if (r.value("target", "") == "binaural") binaural2 = (r.value("channels", -1) == 2);
            if (r.value("target", "") == "ambix") ambiOrder = r.contains("ambisonicOrder");
        }
        check(binaural2, "binaural master is a 2-channel downmix");
        check(ambiOrder, "ambix master reports an ambisonic order");
        check(sc.contains("atmos"), "render_deliverables returns the Atmos send layout");
        check(sc["atmos"].value("bedChannels", 0) == 12, "Atmos 7.1.4 bed = 12 channels");
        check(sc["atmos"].value("objectCount", 0) == 3, "Atmos layout counts the 3 object tracks");
        check(sc["atmos"]["channelPlan"].is_array(), "Atmos layout has a channel plan");

        json rd2 = {{"jsonrpc", "2.0"}, {"id", 137}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.render_deliverables"},
                                {"arguments", {{"targets", json::array({"multichannel"})},
                                               {"format", "mp3"}}}}}};
        json rd2r = rpc(cli, cfg.token, rd2);
        check(rd2r["result"]["structuredContent"].value("format", "") == "l3pm",
              "format 'mp3' maps to the l3pm sink id");
    }

    // --- 13d. Phase-4 batch-e binaural monitor + head-tracking (host path: plan shape, OSC config) ---
    {
        // Ambisonic monitor, order 3, with head-tracking requested.
        json bm = {{"jsonrpc", "2.0"}, {"id", 138}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.add_binaural_monitor"},
                               {"arguments", {{"track", 0}, {"order", 3}, {"headTracking", true},
                                              {"oscPort", 9001}}}}}};
        json bmr = rpc(cli, cfg.token, bm);
        check(bmr["result"].value("isError", true) == false, "add_binaural_monitor succeeds (host)");
        const json& sc = bmr["result"]["structuredContent"];
        check(sc.value("mode", "") == "ambisonic", "monitor defaults to ambisonic mode");
        check(sc.value("busChannels", 0) == 16, "order-3 monitor bus is 16ch (HOA carried to decoder)");
        check(sc.value("hoaChannels", 0) == 16, "hoaChannels reports 16 for order 3");
        check(sc.value("decoder", "") == "IEM BinauralDecoder", "decoder is IEM BinauralDecoder");
        check(sc["headTracking"].value("enabled", false) == true, "head-tracking enabled in plan");
        check(sc["headTracking"].value("oscPort", 0) == 9001, "head-track OSC port echoed (9001)");
        check(sc["headTracking"]["listenAddresses"].is_array()
                  && sc["headTracking"]["listenAddresses"].size() >= 4,
              "head-track reports the OSC listen addresses");
        check(sc["automationParams"].contains("yaw"), "rotator yaw param index reported for automation");
        check(sc["sends"].is_array() && !sc["sends"].empty(), "monitor is fed by a send off the source");

        // Object mode with a list of object tracks.
        json bo = {{"jsonrpc", "2.0"}, {"id", 139}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.add_binaural_monitor"},
                               {"arguments", {{"track", 0}, {"mode", "object"},
                                              {"objectTracks", json::array({3, 4, 5})}}}}}};
        json bor = rpc(cli, cfg.token, bo);
        const json& so = bor["result"]["structuredContent"];
        check(bor["result"].value("isError", true) == false, "add_binaural_monitor object mode succeeds");
        check(so.value("mode", "") == "object", "object mode echoed");
        check(so.value("decoder", "") == "SPARTA Binauraliser", "object mode uses SPARTA Binauraliser");
        check(so.value("inputPins", 0) == 3, "one input pin per object (3)");
        check(so["sends"].is_array() && so["sends"].size() == 3, "one send per object track");

        // stop_head_tracking is always safe (idempotent).
        json st = {{"jsonrpc", "2.0"}, {"id", 141}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.stop_head_tracking"}, {"arguments", json::object()}}}};
        json str = rpc(cli, cfg.token, st);
        check(str["result"].value("isError", true) == false, "stop_head_tracking succeeds");
        check(str["result"]["structuredContent"].value("ok", false) == true, "stop_head_tracking ok");
    }

    // --- 13e. Phase-5b composite spatial verbs (host path: dispatch + plan shape + structured errors) ---
    {
        // spatialize_stems into a 7.1.4 bed by descriptor, with a monitor.
        json sp = {{"jsonrpc", "2.0"}, {"id", 150}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.spatialize_stems"},
                               {"arguments", {{"sources", json::array({3, 4, "Sub"})},
                                              {"target", {{"layout", "7.1.4"}}},
                                              {"placements", json::array({"wide", "front-center", "lfe"})},
                                              {"monitor", true}}}}}};
        json spr = rpc(cli, cfg.token, sp);
        check(spr["result"].value("isError", true) == false, "spatialize_stems dispatches (host)");
        const json& sc = spr["result"]["structuredContent"];
        check(sc.value("ok", false) == true, "spatialize_stems ok");
        check(sc.value("target", "").find("7.1.4 bed") != std::string::npos, "target echoes the 7.1.4 bed");
        check(sc.value("busName", "") == "7.1.4 Bed", "default busName derived from layout");
        check(sc.contains("plan") && sc["plan"].is_array() && !sc["plan"].empty(), "spatialize plan present");
        check(sc.contains("diff") && sc["diff"].is_string(), "spatialize human diff present");

        // Ambisonic target dryRun.
        json spa = {{"jsonrpc", "2.0"}, {"id", 151}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.spatialize_stems"},
                                {"arguments", {{"sources", json::array({2})},
                                               {"target", {{"ambisonicOrder", 3}}},
                                               {"placements", json::array({"overhead"})},
                                               {"dryRun", true}}}}}};
        json spar = rpc(cli, cfg.token, spa);
        const json& sca = spar["result"]["structuredContent"];
        check(sca.value("dryRun", false) == true, "ambisonic spatialize dryRun marks dryRun");
        check(sca.value("target", "").find("3-order ambisonics") != std::string::npos,
              "ambisonic target reports order 3");

        // Unknown descriptor -> structured, agent-actionable error (not an exception).
        json spe = {{"jsonrpc", "2.0"}, {"id", 152}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.spatialize_stems"},
                                {"arguments", {{"sources", json::array({0})},
                                               {"target", {{"layout", "7.1.4"}}},
                                               {"placements", json::array({"banana"})}}}}}};
        json sper = rpc(cli, cfg.token, spe);
        check(sper["result"].value("isError", true) == false, "unknown descriptor is a structured result, not an error");
        check(sper["result"]["structuredContent"].value("error", "") == "unknown_descriptor",
              "unknown descriptor -> error code unknown_descriptor");
        check(sper["result"]["structuredContent"].contains("remediation"), "structured error carries remediation");

        // Missing target key -> structured error.
        json spt = {{"jsonrpc", "2.0"}, {"id", 153}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.spatialize_stems"},
                                {"arguments", {{"sources", json::array({0})}, {"target", json::object()}}}}}};
        json sptr = rpc(cli, cfg.token, spt);
        check(sptr["result"]["structuredContent"].value("error", "") == "no_target",
              "empty target -> no_target error");

        // stereo_to_ambisonic (host): order 1 sketch.
        json sa = {{"jsonrpc", "2.0"}, {"id", 154}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.stereo_to_ambisonic"},
                               {"arguments", {{"source", 1}, {"order", 1}}}}}};
        json sar = rpc(cli, cfg.token, sa);
        const json& scs = sar["result"]["structuredContent"];
        check(sar["result"].value("isError", true) == false, "stereo_to_ambisonic dispatches (host)");
        check(scs.value("order", 0) == 1 && scs.value("channels", 0) == 4, "order 1 -> 4 channels");
        check(scs.value("sketch", false) == true, "stereo_to_ambisonic is labelled a sketch");

        // setup_immersive_session (host): bed + objects + renderer.
        json ss = {{"jsonrpc", "2.0"}, {"id", 155}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.setup_immersive_session"},
                               {"arguments", {{"bed", "7.1.4"}, {"objectCount", 4},
                                              {"monitor", true}, {"rendererSends", true}}}}}};
        json ssr = rpc(cli, cfg.token, ss);
        const json& scss = ssr["result"]["structuredContent"];
        check(ssr["result"].value("isError", true) == false, "setup_immersive_session dispatches (host)");
        check(scss.value("bedLayout", "") == "7.1.4", "session bed layout echoed");
        check(scss["objectTracks"].is_array() && scss["objectTracks"].size() == 4, "4 object tracks planned");
        check(scss.contains("plan") && scss["plan"].is_array(), "session plan present");

        // The three new verbs are all visible under the spatial profile.
        json tlist = {{"jsonrpc", "2.0"}, {"id", 156}, {"method", "tools/list"}, {"params", json::object()}};
        json tlr = rpc(cli, cfg.token, tlist);
        int sawVerbs = 0;
        for (const auto& t : tlr["result"]["tools"]) {
            const std::string n = t.value("name", "");
            if (n == "spatial.spatialize_stems" || n == "spatial.stereo_to_ambisonic" ||
                n == "spatial.setup_immersive_session") ++sawVerbs;
        }
        check(sawVerbs == 3, "all 3 Phase-5b composite verbs listed under the spatial profile");
    }

    // --- 13f. Phase-5c/d: analysis.check_deliverable + mix.apply_style (host dispatch + judgement) ---
    {
        // check_deliverable evaluates SUPPLIED measurements (no render needed, so it works host-side).
        // ebu-r128 at -23.0 / -1.5 dBTP -> pass.
        json cd = {{"jsonrpc", "2.0"}, {"id", 160}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.check_deliverable"},
                               {"arguments", {{"spec", "ebu-r128"},
                                              {"measured", {{"lufsIntegrated", -23.0}, {"truePeak", -1.5}}}}}}}};
        json cdr = rpc(cli, cfg.token, cd);
        check(cdr["result"].value("isError", true) == false, "check_deliverable dispatches (host)");
        const json& cs = cdr["result"]["structuredContent"];
        check(cs.value("pass", false) == true, "ebu-r128 -23.0/-1.5 -> pass");
        check(cs.value("measuredSource", "") == "supplied", "supplied-measurement mode");

        // Over-loud + over-ceiling -> fail.
        json cd2 = {{"jsonrpc", "2.0"}, {"id", 161}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.check_deliverable"},
                                {"arguments", {{"spec", "atsc-a85"},
                                               {"measured", {{"lufsIntegrated", -21.0}, {"truePeak", -0.5}}}}}}}};
        json cd2r = rpc(cli, cfg.token, cd2);
        check(cd2r["result"]["structuredContent"].value("pass", true) == false,
              "atsc-a85 -21.0/-0.5 -> fail (too loud + over -2 dBTP)");

        // cinema-theatrical: LUFS not gated, TP gates.
        json cd3 = {{"jsonrpc", "2.0"}, {"id", 162}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.check_deliverable"},
                                {"arguments", {{"spec", "cinema-theatrical"},
                                               {"measured", {{"lufsIntegrated", -30.0}, {"truePeak", -3.4}}}}}}}};
        check(rpc(cli, cfg.token, cd3)["result"]["structuredContent"].value("pass", false) == true,
              "cinema-theatrical passes on TP alone (LUFS not gated)");

        // Unknown spec -> structured, agent-actionable error.
        json cde = {{"jsonrpc", "2.0"}, {"id", 163}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.check_deliverable"},
                                {"arguments", {{"spec", "banana"}}}}}};
        json cder = rpc(cli, cfg.token, cde);
        check(cder["result"].value("isError", true) == false, "unknown spec is a structured result");
        check(cder["result"]["structuredContent"].value("error", "") == "unknown_spec",
              "unknown spec -> error code unknown_spec");
        check(cder["result"]["structuredContent"].contains("remediation"), "carries remediation");

        // apply_style (host): resolves the chain plan and reports it.
        json ms = {{"jsonrpc", "2.0"}, {"id", 164}, {"method", "tools/call"},
                   {"params", {{"name", "mix.apply_style"},
                               {"arguments", {{"track", 0}, {"style", "pop-bright"}}}}}};
        json msr = rpc(cli, cfg.token, ms);
        check(msr["result"].value("isError", true) == false, "apply_style dispatches (host)");
        const json& mc = msr["result"]["structuredContent"];
        check(mc.value("ok", false) == true, "apply_style ok");
        check(mc.value("style", "") == "pop-bright", "style echoed");
        check(mc.contains("steps") && mc["steps"].is_array() && !mc["steps"].empty(), "resolved chain steps");
        // in-box fallback (nothing 'installed' host-side) + limiter ceiling from the style default spec.
        bool sawInboxLimiter = false;
        for (const auto& s : mc["steps"])
            if (s.value("isLimiter", false) && s.value("fx", "") == "ReaLimit") sawInboxLimiter = true;
        check(sawInboxLimiter, "host resolves the in-box ReaLimit limiter");

        // targetSpec overrides the ceiling.
        json ms2 = {{"jsonrpc", "2.0"}, {"id", 165}, {"method", "tools/call"},
                    {"params", {{"name", "mix.apply_style"},
                                {"arguments", {{"track", 0}, {"style", "immersive-master-atmos"},
                                               {"targetSpec", "atmos-music"}}}}}};
        json ms2r = rpc(cli, cfg.token, ms2);
        check(ms2r["result"]["structuredContent"].value("ceilingDb", 0.0) == -1.0,
              "atmos-music targetSpec -> -1 dBTP limiter ceiling");
        check(ms2r["result"]["structuredContent"].value("immersiveAware", false) == true,
              "immersive master reports immersiveAware");

        // Unknown style -> structured error.
        json mse = {{"jsonrpc", "2.0"}, {"id", 166}, {"method", "tools/call"},
                    {"params", {{"name", "mix.apply_style"},
                                {"arguments", {{"track", 0}, {"style", "banana"}}}}}};
        check(rpc(cli, cfg.token, mse)["result"]["structuredContent"].value("error", "") == "unknown_style",
              "unknown style -> error code unknown_style");

        // --- Phase-6b: per-bed sub-metering via SUPPLIED measurement array (no render; host path) ---
        json pb = {{"jsonrpc", "2.0"}, {"id", 168}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.check_deliverable"},
                     {"arguments", {{"spec", "atmos-music"},
                       {"measured", json::array({
                          {{"target", "Music Bed"}, {"lufsIntegrated", -18.1}, {"truePeak", -1.3}, {"channels", 12}},
                          {{"target", "FX Bed"},    {"lufsIntegrated", -11.5}, {"truePeak", -0.2}, {"channels", 12}}
                       })}}}}}};
        json pbr = rpc(cli, cfg.token, pb);
        check(pbr["result"].value("isError", true) == false, "per-bed (supplied array) dispatches (host)");
        const json& pbc = pbr["result"]["structuredContent"];
        check(pbc.contains("perBed") && pbc["perBed"].size() == 2, "per-bed reports two beds");
        check(pbc["perBed"][0].value("pass", false) == true, "bed 0 (Music Bed) passes");
        check(pbc["perBed"][1].value("pass", true) == false, "bed 1 (FX Bed) fails (too loud + over ceiling)");
        check(pbc["rollup"].value("pass", true) == false, "rollup fails when any bed fails");
        check(pbc["rollup"]["failing"].size() == 1 && pbc["rollup"]["failing"][0] == "FX Bed",
              "rollup names the failing bed");
        check(pbc.value("measuredSource", "") == "supplied", "per-bed supplied source");

        // --- Phase-6c: apply_style host resolution picks the multichannel limiter on a wide bed ---
        json mcw = {{"jsonrpc", "2.0"}, {"id", 169}, {"method", "tools/call"},
                    {"params", {{"name", "mix.apply_style"},
                      {"arguments", {{"track", 0}, {"style", "immersive-master-atmos"},
                                     {"targetSpec", "atmos-music"}, {"channels", 12}}}}}};
        json mcwr = rpc(cli, cfg.token, mcw);
        check(mcwr["result"].value("isError", true) == false, "wide-bed apply_style dispatches (host)");
        const json& mcwc = mcwr["result"]["structuredContent"];
        bool sawMcLimiter = false;
        for (const auto& s : mcwc["steps"])
            if (s.value("isLimiter", false) && s.value("multichannel", false)) sawMcLimiter = true;
        check(sawMcLimiter, "wide immersive bed (channels=12) resolves the multichannel limiter");
        // A stereo immersive target keeps the stock 2-ch limiter (no multichannel swap).
        json mcn = {{"jsonrpc", "2.0"}, {"id", 170}, {"method", "tools/call"},
                    {"params", {{"name", "mix.apply_style"},
                      {"arguments", {{"track", 0}, {"style", "immersive-master-atmos"},
                                     {"channels", 2}}}}}};
        json mcnr = rpc(cli, cfg.token, mcn);
        bool sawStockLimiter = false;
        for (const auto& s : mcnr["result"]["structuredContent"]["steps"])
            if (s.value("isLimiter", false) && !s.value("multichannel", false) && s.value("fx", "") == "ReaLimit")
                sawStockLimiter = true;
        check(sawStockLimiter, "stereo immersive target keeps the stock ReaLimit (no MC swap)");

        // Both new tools are visible on the default (full) surface, under their profiles.
        json tl = {{"jsonrpc", "2.0"}, {"id", 167}, {"method", "tools/list"}, {"params", json::object()}};
        json tlr = rpc(cli, cfg.token, tl);
        bool sawAnalysis = false, sawMix = false;
        for (const auto& t : tlr["result"]["tools"]) {
            const std::string n = t.value("name", "");
            if (n == "analysis.check_deliverable") sawAnalysis = true;
            if (n == "mix.apply_style") sawMix = true;
        }
        check(sawAnalysis, "analysis.check_deliverable listed");
        check(sawMix, "mix.apply_style listed");
    }

    // --- 14. Resource provider: list / templates / read (routing graph + track chunk template) ---
    {
        json lreq = {{"jsonrpc", "2.0"}, {"id", 140}, {"method", "resources/list"},
                     {"params", json::object()}};
        json lr = rpc(cli, cfg.token, lreq);
        const json& res = lr["result"]["resources"];
        check(res.is_array() && res.size() >= 2, "resources/list returns the static resources (>=2)");
        bool sawState = false, sawGraph = false;
        for (const auto& r : res) {
            if (r.value("uri", "") == "reaper://project/state") sawState = true;
            if (r.value("uri", "") == "reaper://routing/graph") sawGraph = true;
            check(r.contains("mimeType"), "resource entry has mimeType");
        }
        check(sawState && sawGraph, "project/state + routing/graph resources listed");

        json treq = {{"jsonrpc", "2.0"}, {"id", 141}, {"method", "resources/templates/list"},
                     {"params", json::object()}};
        json tr = rpc(cli, cfg.token, treq);
        bool sawChunkTmpl = false;
        for (const auto& t : tr["result"]["resourceTemplates"])
            if (t.value("uriTemplate", "") == "reaper://track/{index}/chunk") sawChunkTmpl = true;
        check(sawChunkTmpl, "resources/templates/list advertises the track chunk template");

        json rreq = {{"jsonrpc", "2.0"}, {"id", 142}, {"method", "resources/read"},
                     {"params", {{"uri", "reaper://routing/graph"}}}};
        json rr = rpc(cli, cfg.token, rreq);
        check(rr.contains("result"), "resources/read(routing/graph) has result");
        const json& contents = rr["result"]["contents"];
        check(contents.is_array() && !contents.empty(), "resources/read returns contents[]");
        check(contents[0].value("mimeType", "") == "application/json", "routing graph is application/json");
        json graph = json::parse(contents[0].value("text", "{}"), nullptr, false);
        check(graph.is_object() && graph.contains("nodes") && graph.contains("edges"),
              "routing graph text parses to {nodes, edges}");

        json creq = {{"jsonrpc", "2.0"}, {"id", 143}, {"method", "resources/read"},
                     {"params", {{"uri", "reaper://track/0/chunk"}}}};
        json crr = rpc(cli, cfg.token, creq);
        check(crr.contains("result"), "resources/read(track/0/chunk) matches the template");
        check(crr["result"]["contents"][0].value("mimeType", "") == "text/plain",
              "track chunk resource is text/plain");

        json bad = {{"jsonrpc", "2.0"}, {"id", 144}, {"method", "resources/read"},
                    {"params", {{"uri", "reaper://does/not/exist"}}}};
        json badr = rpc(cli, cfg.token, bad);
        check(badr.contains("error") && badr["error"].value("code", 0) == -32602,
              "unknown resource -> JSON-RPC error -32602");
    }

    // --- 15. Resource subscriptions (Phase 3): subscribe -> SSE notifications/resources/updated,
    //         coalescing, and unsubscribe stops delivery. The SSE stream runs on a background thread;
    //         a REAPER-side change is simulated by calling subs.markDirty() directly (the control
    //         surface would do this from a Set* callback on the main thread). ---
    std::mutex sseMu;
    std::string sseBuf;
    std::atomic<bool> sseKeep{true};
    httplib::Client sseCli("127.0.0.1", port);
    sseCli.set_read_timeout(30, 0);
    std::thread sseThread([&] {
        httplib::Headers h = {{"Authorization", "Bearer " + cfg.token}};
        sseCli.Get("/mcp", h, [&](const char* data, size_t len) -> bool {
            std::lock_guard<std::mutex> lk(sseMu);
            sseBuf.append(data, len);
            return sseKeep.load();
        });
    });
    {
        auto sseHas = [&](const std::string& needle) {
            std::lock_guard<std::mutex> lk(sseMu);
            return sseBuf.find(needle) != std::string::npos;
        };
        auto countUpdated = [&]() {
            std::lock_guard<std::mutex> lk(sseMu);
            size_t n = 0, pos = 0;
            static const std::string key = "notifications/resources/updated";
            while ((pos = sseBuf.find(key, pos)) != std::string::npos) { ++n; pos += key.size(); }
            return n;
        };

        std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let the SSE stream establish

        // subscribe validation
        {
            json req = {{"jsonrpc", "2.0"}, {"id", 150}, {"method", "resources/subscribe"},
                        {"params", json::object()}};
            json resp = rpc(cli, cfg.token, req);
            check(resp.contains("error") && resp["error"].value("code", 0) == -32602,
                  "resources/subscribe without uri -> -32602");

            json req2 = {{"jsonrpc", "2.0"}, {"id", 151}, {"method", "resources/subscribe"},
                         {"params", {{"uri", "reaper://does/not/exist"}}}};
            json resp2 = rpc(cli, cfg.token, req2);
            check(resp2.contains("error") && resp2["error"].value("code", 0) == -32602,
                  "resources/subscribe to unknown resource -> -32602");
        }

        // subscribe to a static resource and to a concrete instance of the chunk template
        {
            json a = {{"jsonrpc", "2.0"}, {"id", 152}, {"method", "resources/subscribe"},
                      {"params", {{"uri", "reaper://project/state"}}}};
            check(rpc(cli, cfg.token, a).contains("result"), "resources/subscribe(project/state) acked");

            json b = {{"jsonrpc", "2.0"}, {"id", 153}, {"method", "resources/subscribe"},
                      {"params", {{"uri", "reaper://track/0/chunk"}}}};
            check(rpc(cli, cfg.token, b).contains("result"),
                  "resources/subscribe(track/0/chunk template instance) acked");
        }

        // simulate REAPER-side changes; a burst on one URI must coalesce to a single notification
        for (int i = 0; i < 50; ++i) subs.markDirty("reaper://project/state");

        bool got = false;
        for (int i = 0; i < 80 && !got; ++i) {
            if (sseHas("notifications/resources/updated") && sseHas("reaper://project/state")) got = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(got, "SSE delivered notifications/resources/updated for the subscribed resource");
        check(countUpdated() == 1, "a burst of marks on one resource coalesced to a single notification");

        // a change to a resource nobody subscribed to must not be delivered
        subs.markDirty("reaper://routing/graph");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        check(!sseHas("reaper://routing/graph"), "unsubscribed resource change is not delivered");

        // unsubscribe -> subsequent changes produce no further notifications
        {
            json u = {{"jsonrpc", "2.0"}, {"id", 154}, {"method", "resources/unsubscribe"},
                      {"params", {{"uri", "reaper://project/state"}}}};
            check(rpc(cli, cfg.token, u).contains("result"), "resources/unsubscribe(project/state) acked");
        }
        const size_t before = countUpdated();
        subs.markDirty("reaper://project/state");  // no-op now (unsubscribed)
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        check(countUpdated() == before,
              "after unsubscribe, further changes produce no new notifications");
    }

    // --- 16. MCP prompts (Phase 5a): list + get, argument substitution, and validation errors ---
    {
        json lreq = {{"jsonrpc", "2.0"}, {"id", 160}, {"method", "prompts/list"},
                     {"params", json::object()}};
        json lr = rpc(cli, cfg.token, lreq);
        check(lr.contains("result"), "prompts/list has result");
        const json& prompts = lr["result"]["prompts"];
        check(prompts.is_array() && prompts.size() == 3, "prompts/list returns the 3 expert workflows");
        // std::map -> deterministic alphabetical order.
        check(prompts[0].value("name", "") == "encode_to_ambisonics", "prompt[0] = encode_to_ambisonics");
        check(prompts[1].value("name", "") == "master_for_delivery", "prompt[1] = master_for_delivery");
        check(prompts[2].value("name", "") == "setup_atmos_session", "prompt[2] = setup_atmos_session");
        bool sawRequired = false, sawTitle = false;
        for (const auto& p : prompts) {
            check(p.contains("description") && p["arguments"].is_array(), "prompt has description+arguments");
            if (!p.value("title", "").empty()) sawTitle = true;
            for (const auto& a : p["arguments"])
                if (a.value("name", "") == "sourceBus" && a.value("required", false)) sawRequired = true;
        }
        check(sawTitle, "prompts advertise a human title");
        check(sawRequired, "encode_to_ambisonics.sourceBus is a required argument");

        // get with argument substitution
        json greq = {{"jsonrpc", "2.0"}, {"id", 161}, {"method", "prompts/get"},
                     {"params", {{"name", "setup_atmos_session"},
                                 {"arguments", {{"beds", "9.1.6"}, {"objects", "6"}}}}}};
        json gr = rpc(cli, cfg.token, greq);
        check(gr.contains("result"), "prompts/get(setup_atmos_session) has result");
        const json& msgs = gr["result"]["messages"];
        check(msgs.is_array() && !msgs.empty(), "prompts/get returns messages[]");
        check(msgs[0].value("role", "") == "user", "prompt message role is user");
        check(msgs[0]["content"].value("type", "") == "text", "prompt message content is text");
        const std::string text = msgs[0]["content"].value("text", "");
        check(text.find("9.1.6") != std::string::npos, "prompt substitutes the bed layout argument (9.1.6)");
        check(text.find("setup_immersive_session") != std::string::npos,
              "prompt steers the agent to setup_immersive_session");

        // get with a defaulted optional argument (no beds -> default 7.1.4)
        json gdef = {{"jsonrpc", "2.0"}, {"id", 162}, {"method", "prompts/get"},
                     {"params", {{"name", "setup_atmos_session"}, {"arguments", json::object()}}}};
        json gdr = rpc(cli, cfg.token, gdef);
        check(gdr["result"]["messages"][0]["content"].value("text", "").find("7.1.4") != std::string::npos,
              "optional argument falls back to its documented default (7.1.4)");

        // missing required argument -> -32602
        json gbad = {{"jsonrpc", "2.0"}, {"id", 163}, {"method", "prompts/get"},
                     {"params", {{"name", "encode_to_ambisonics"}, {"arguments", json::object()}}}};
        json gbadr = rpc(cli, cfg.token, gbad);
        check(gbadr.contains("error") && gbadr["error"].value("code", 0) == -32602,
              "prompts/get missing required arg -> -32602");

        // unknown prompt -> -32602
        json gunk = {{"jsonrpc", "2.0"}, {"id", 164}, {"method", "prompts/get"},
                     {"params", {{"name", "no_such_prompt"}}}};
        json gunkr = rpc(cli, cfg.token, gunk);
        check(gunkr.contains("error") && gunkr["error"].value("code", 0) == -32602,
              "prompts/get unknown name -> -32602");
    }

    // --- 17. Phase 6 (d): MCP elicitation — capability parse, the confirm:true fallback, and the
    //         full async round-trip (SSE-answered tools/call -> elicitation/create -> correlated
    //         response POST -> resume) over real HTTP, plus decline/timeout/stray-response paths. ---
    {
        // Extract all JSON messages carried on `data:` SSE lines from an accumulated stream buffer.
        // json::dump() emits no newlines, so each data line is exactly one message.
        auto frames = [](const std::string& s) {
            std::vector<json> out;
            const std::string key = "data: ";
            size_t pos = 0;
            while ((pos = s.find(key, pos)) != std::string::npos) {
                const size_t start = pos + key.size();
                const size_t nl = s.find('\n', start);
                const std::string line = (nl == std::string::npos) ? s.substr(start)
                                                                    : s.substr(start, nl - start);
                json j = json::parse(line, nullptr, false);
                if (!j.is_discarded()) out.push_back(std::move(j));
                pos = (nl == std::string::npos) ? s.size() : nl + 1;
            }
            return out;
        };

        // --- 17a. fallback: a client WITHOUT elicitation (the section-1 initialize sent empty caps)
        //          gets the confirm:true stopgap, returned as a normal (non-SSE) JSON body. ---
        {
            json call = {{"jsonrpc", "2.0"}, {"id", 900}, {"method", "tools/call"},
                         {"params", {{"name", "test.elicit_confirm"}, {"arguments", json::object()}}}};
            json resp = rpc(cli, cfg.token, call);
            check(resp.contains("result"), "no-elicitation: single tools/call returns a normal JSON body");
            const json& sc = resp["result"]["structuredContent"];
            check(sc.value("error", "") == "confirmation_required",
                  "no-elicitation: destructive verb falls back to confirmation_required");

            json ok = {{"jsonrpc", "2.0"}, {"id", 901}, {"method", "tools/call"},
                       {"params", {{"name", "test.elicit_confirm"}, {"arguments", {{"confirm", true}}}}}};
            json okr = rpc(cli, cfg.token, ok);
            check(okr["result"]["structuredContent"].value("confirmed", false),
                  "no-elicitation: confirm:true proceeds");
        }

        // --- 17b. re-initialize advertising elicitation -> the server records the capability. ---
        {
            json init = {{"jsonrpc", "2.0"}, {"id", 902}, {"method", "initialize"},
                         {"params", {{"protocolVersion", "2025-06-18"},
                                     {"capabilities", {{"elicitation", json::object()}}},
                                     {"clientInfo", {{"name", "test-elicit"}, {"version", "0"}}}}}};
            json r = rpc(cli, cfg.token, init);
            check(r.contains("result"), "initialize with capabilities.elicitation acked");
        }

        // Drive one elicited tools/call: stream the call on a background thread, read the
        // elicitation/create id, answer it, then collect the final tools/call result frame.
        auto runElicited = [&](int callId, const std::string& action, bool sendAnswer,
                               json* resultOut) -> std::string {
            auto buf = std::make_shared<std::string>();
            auto bmu = std::make_shared<std::mutex>();
            httplib::Client streamCli("127.0.0.1", port);
            streamCli.set_read_timeout(10, 0);
            json call = {{"jsonrpc", "2.0"}, {"id", callId}, {"method", "tools/call"},
                         {"params", {{"name", "test.elicit_confirm"}, {"arguments", {{"note", "hi"}}}}}};
            std::thread th([&, buf, bmu] {
                httplib::Headers h = {{"Authorization", "Bearer " + cfg.token}};
                streamCli.Post("/mcp", h, call.dump(), "application/json",
                               [&, buf, bmu](const char* d, size_t n) -> bool {
                                   std::lock_guard<std::mutex> lk(*bmu);
                                   buf->append(d, n);
                                   return true;
                               });
            });

            // wait for the elicitation/create frame and extract its id
            std::string elicId;
            for (int i = 0; i < 300 && elicId.empty(); ++i) {
                {
                    std::lock_guard<std::mutex> lk(*bmu);
                    for (const auto& f : frames(*buf))
                        if (f.value("method", "") == "elicitation/create") elicId = f.value("id", "");
                }
                if (elicId.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (sendAnswer && !elicId.empty()) {
                json ans = {{"jsonrpc", "2.0"}, {"id", elicId},
                            {"result", {{"action", action}, {"content", {{"confirm", action == "accept"}}}}}};
                int st = 0;
                rpc(cli, cfg.token, ans, &st);
                check(st == 202, "elicitation answer POST accepted (202, no reply body)");
            }

            th.join();  // stream closes once the server writes the final result frame
            // pull the tools/call result frame (id == callId) out of the buffer
            std::lock_guard<std::mutex> lk(*bmu);
            for (const auto& f : frames(*buf))
                if (f.contains("result") && f.value("id", -1) == callId) *resultOut = f["result"];
            return elicId;
        };

        // --- 17c. accept -> the mutation runs, confirm:true was folded in. ---
        {
            json result;
            std::string elicId = runElicited(910, "accept", /*sendAnswer=*/true, &result);
            check(!elicId.empty(), "accept: server streamed elicitation/create with an id");
            check(elicId.rfind("elic-", 0) == 0, "elicitation id has the server-minted 'elic-' prefix");
            check(!result.is_null(), "accept: server streamed the tools/call result on the SSE stream");
            check(result.value("isError", true) == false, "accept: result is not an error");
            check(result["structuredContent"].value("confirmed", false),
                  "accept: the mutation ran (confirmed:true) after the human accepted");
            check(result["structuredContent"].value("note", "") == "hi",
                  "accept: original arguments survived the re-invocation (note echoed)");
        }

        // --- 17d. decline -> no mutation, an isError result. ---
        {
            json result;
            runElicited(911, "decline", /*sendAnswer=*/true, &result);
            check(!result.is_null(), "decline: server streamed a result");
            check(result.value("isError", false) == true, "decline: result isError:true (no mutation)");
            const std::string txt = result.contains("content") && !result["content"].empty()
                                        ? result["content"][0].value("text", "")
                                        : "";
            check(txt.find("declined") != std::string::npos, "decline: message says the user declined");
        }

        // --- 17e. timeout -> no answer within elicitationTimeoutMs -> isError, no mutation. ---
        {
            json result;
            runElicited(912, "", /*sendAnswer=*/false, &result);  // never answer
            check(!result.is_null(), "timeout: server streamed a result after the cap");
            check(result.value("isError", false) == true, "timeout: result isError:true (no mutation)");
            const std::string txt = result.contains("content") && !result["content"].empty()
                                        ? result["content"][0].value("text", "")
                                        : "";
            check(txt.find("timed out") != std::string::npos, "timeout: message says it timed out");
        }

        // --- 17f. a stray response for an unknown id is accepted (202) and harmlessly ignored. ---
        {
            json stray = {{"jsonrpc", "2.0"}, {"id", "elic-does-not-exist"},
                          {"result", {{"action", "accept"}, {"content", {{"confirm", true}}}}}};
            int st = 0;
            rpc(cli, cfg.token, stray, &st);
            check(st == 202, "stray elicitation response -> 202, ignored (no waiter)");
        }
    }

    // --- 2l. Batch O1: native ADM object-audio authoring (export_adm / author_object_bed /
    //     adm_inspect). Host build cannot render; these prove presence, dispatch, required-arg
    //     contracts, and host-path shapes. The chna/axml/RIFF/BW64 serialization + round-trip parser
    //     are proven in unit.adm.
    {
        json listReq = {{"jsonrpc", "2.0"}, {"id", 930}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("spatial.export_adm") && listHas("spatial.author_object_bed") &&
              listHas("analysis.adm_inspect"),
              "all three Batch-O1 ADM tools present in tools/list");
    }
    {
        // (ii) export_adm host-path shape: bed + objects -> channel plan (no render off-REAPER).
        json req = {{"jsonrpc", "2.0"}, {"id", 931}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.export_adm"},
                                {"arguments", {{"bedTrack", 0}, {"bedLayout", "7.1.2"},
                                               {"objectTracks", {1, 2}}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false, "spatial.export_adm dispatches (host)");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("bedLayout", "") == "7.1.2" && sc.value("bedChannels", 0) == 10 &&
              sc.value("objectCount", 0) == 2 && sc.value("channels", 0) == 12,
              "export_adm plans 7.1.2 bed (10) + 2 objects = 12 channels");
    }
    {
        // (iii) export_adm required source: neither bed nor objects is fine off-REAPER (host stub),
        //     but the coordinateMode echo proves the arg surface flows through.
        json req = {{"jsonrpc", "2.0"}, {"id", 932}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.export_adm"},
                                {"arguments", {{"objectTracks", {3}}, {"coordinateMode", "cartesian"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("coordinateMode", "") == "cartesian" && sc.value("objectCount", 0) == 1,
              "export_adm echoes coordinateMode + object count (objects-only)");
    }
    {
        // (iv) author_object_bed host-path shape: reports bed + object map + ready flag.
        json req = {{"jsonrpc", "2.0"}, {"id", 933}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.author_object_bed"},
                                {"arguments", {{"bedTrack", 0}, {"bedLayout", "7.1.4"},
                                               {"objectTracks", {1, 2, 3}}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "spatial.author_object_bed dispatches (host)");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.contains("bed") && sc.contains("objects") && sc.value("objects", json::array()).size() == 3,
              "author_object_bed returns bed + a 3-object map");
        check(sc.value("dryRun", false) == true, "author_object_bed defaults to dryRun");
    }
    {
        // (v) adm_inspect required-arg contract (path) + not-found error path.
        json bad = {{"jsonrpc", "2.0"}, {"id", 934}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.adm_inspect"}, {"arguments", json::object()}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false) == true,
              "analysis.adm_inspect without path -> isError");
        json miss = {{"jsonrpc", "2.0"}, {"id", 935}, {"method", "tools/call"},
                     {"params", {{"name", "analysis.adm_inspect"},
                                 {"arguments", {{"path", "/nonexistent/_mcp_no_such_adm.wav"}}}}}};
        json missResp = rpc(cli, cfg.token, miss);
        const json& sc = missResp["result"]["structuredContent"];
        check(sc.contains("error") && sc.value("error", "") == "file_not_found",
              "analysis.adm_inspect on a missing file returns file_not_found");
    }
    {
        // (vi) Batch O2: the per-object metadata surface flows through export_adm. Off-REAPER the tool
        //      can't render, but the host path echoes objectMetadataCount so the arg surface is proven;
        //      the extent/objectDivergence/importance serialization + round-trip live in unit.adm.
        json req = {{"jsonrpc", "2.0"}, {"id", 936}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.export_adm"},
                                {"arguments", {{"bedTrack", 0}, {"bedLayout", "7.1.2"},
                                               {"objectTracks", {1, 2}},
                                               {"objectMetadata",
                                                {{{"track", 1}, {"size", 0.3}, {"divergence", 0.5},
                                                  {"importance", 8}},
                                                 {{"track", 2}, {"width", 20.0}}}}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "spatial.export_adm with objectMetadata dispatches (host)");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("objectMetadataCount", 0) == 2,
              "export_adm echoes objectMetadataCount = 2 (O2 arg surface flows through)");
    }
    {
        // (vii) Batch O3: the Dolby Atmos Master ADM Profile surface. Presence of adm_profile_check;
        //       export_adm's profile:"dolby-atmos" mode echoes a profile block + forces cartesian; the
        //       normalize/validate math lives in unit.adm_profile.
        json listReq = {{"jsonrpc", "2.0"}, {"id", 939}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        bool present = false;
        for (const auto& t : listResp["result"]["tools"])
            if (t.value("name", "") == "analysis.adm_profile_check") { present = true; break; }
        check(present, "analysis.adm_profile_check present in tools/list");

        json req = {{"jsonrpc", "2.0"}, {"id", 940}, {"method", "tools/call"},
                    {"params", {{"name", "spatial.export_adm"},
                                {"arguments", {{"bedTrack", 0}, {"bedLayout", "7.1.2"},
                                               {"objectTracks", {1, 2}}, {"profile", "dolby-atmos"}}}}}};
        json resp = rpc(cli, cfg.token, req);
        check(resp["result"].value("isError", false) == false,
              "spatial.export_adm profile:dolby-atmos dispatches (host)");
        const json& sc = resp["result"]["structuredContent"];
        check(sc.value("coordinateMode", "") == "cartesian", "profile mode reports cartesian coordinates");
        check(sc.contains("profile") && sc["profile"].is_object() &&
              sc["profile"].value("conformant", false) == true,
              "export_adm echoes a conformant profile block (host plan)");

        json bad = {{"jsonrpc", "2.0"}, {"id", 941}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.adm_profile_check"}, {"arguments", json::object()}}}};
        check(rpc(cli, cfg.token, bad)["result"].value("isError", false) == true,
              "analysis.adm_profile_check without path -> isError");
        json miss = {{"jsonrpc", "2.0"}, {"id", 942}, {"method", "tools/call"},
                     {"params", {{"name", "analysis.adm_profile_check"},
                                 {"arguments", {{"path", "/nonexistent/_mcp_no_such_adm.wav"}}}}}};
        json missResp = rpc(cli, cfg.token, miss);
        const json& msc = missResp["result"]["structuredContent"];
        check(msc.contains("error") && msc.value("error", "") == "file_not_found",
              "analysis.adm_profile_check on a missing file returns file_not_found");
    }

    // --- 2q. Batch B1 (audio accessors): read_samples / accessor_meter / detect_silence ----------
    // Host build has no REAPER accessor, so this exercises presence + dispatch + the host-path shape;
    // the SDK-free DSP (DC/clip/overview/silence scan) is proven in unit.meter and the accessor
    // lifecycle is live-verified (scripts/verify_accessors.py).
    {
        json listReq = {{"jsonrpc", "2.0"}, {"id", 950}, {"method", "tools/list"},
                        {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, listReq);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("analysis.read_samples") && listHas("analysis.accessor_meter") &&
              listHas("analysis.detect_silence"),
              "all three Batch-B1 audio-accessor tools present in tools/list");

        json rs = {{"jsonrpc", "2.0"}, {"id", 951}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.read_samples"}, {"arguments", {{"target", 0}}}}}};
        json rsResp = rpc(cli, cfg.token, rs);
        check(rsResp["result"].value("isError", false) == false, "analysis.read_samples dispatches (host)");
        const json& rsc = rsResp["result"]["structuredContent"];
        check(rsc.value("source", "") == "track" && rsc.contains("measuredSource"),
              "read_samples returns source + measuredSource");

        json tk = {{"jsonrpc", "2.0"}, {"id", 952}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.read_samples"},
                               {"arguments", {{"target", 0}, {"itemIndex", 0}}}}}};
        check(rpc(cli, cfg.token, tk)["result"]["structuredContent"].value("source", "") == "take",
              "read_samples with itemIndex reports the take source");

        json am = {{"jsonrpc", "2.0"}, {"id", 953}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.accessor_meter"}, {"arguments", {{"target", 1}}}}}};
        check(rpc(cli, cfg.token, am)["result"].value("isError", false) == false,
              "analysis.accessor_meter dispatches (host)");

        json dsr = {{"jsonrpc", "2.0"}, {"id", 954}, {"method", "tools/call"},
                    {"params", {{"name", "analysis.detect_silence"},
                                {"arguments", {{"target", 2}, {"thresholdDb", -50}}}}}};
        json dsResp = rpc(cli, cfg.token, dsr);
        check(dsResp["result"].value("isError", false) == false,
              "analysis.detect_silence dispatches (host)");
        check(dsResp["result"]["structuredContent"].value("thresholdDb", 0.0) == -50.0,
              "detect_silence echoes thresholdDb");

        json dr = {{"jsonrpc", "2.0"}, {"id", 955}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.read_samples"},
                               {"arguments", {{"target", 0}, {"dryRun", true}}}}}};
        check(rpc(cli, cfg.token, dr)["result"]["structuredContent"].value("dryRun", false) == true,
              "read_samples dryRun returns a dryRun result");
    }

    // --- 2r. Batch L1 (object send-layout orchestration): orchestrate_sends / send_layout_inspect ---
    // Host build has no REAPER routing, so this exercises presence + dispatch + the host-path roster
    // shape; the SDK-free roster/encode/reconcile/inspect math is proven in unit.send_layout and the
    // live wiring is verified by scripts/verify_send_layout.py.
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 960}, {"method", "tools/list"}, {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, req);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("spatial.orchestrate_sends") && listHas("analysis.send_layout_inspect"),
              "both Batch-L1 send-layout tools present in tools/list");

        json os = {{"jsonrpc", "2.0"}, {"id", 961}, {"method", "tools/call"},
                   {"params", {{"name", "spatial.orchestrate_sends"},
                               {"arguments", {{"rendererTrack", 0}, {"bedTrack", 1}, {"bedLayout", "7.1.2"},
                                              {"objectTracks", {2, 3, 4}}}}}}};
        json osResp = rpc(cli, cfg.token, os);
        const json& osc = osResp["result"]["structuredContent"];
        check(osc.value("ok", false) && osc.value("objectCount", 0) == 3 &&
              osc.value("totalChannels", 0) == 13,
              "orchestrate_sends host roster = 7.1.2 bed + 3 objects -> 13 channels");
        check(osc.value("dryRun", false) == true, "orchestrate_sends defaults to dryRun");

        json si = {{"jsonrpc", "2.0"}, {"id", 962}, {"method", "tools/call"},
                   {"params", {{"name", "analysis.send_layout_inspect"},
                               {"arguments", {{"rendererTrack", 0}, {"bedLayout", "7.1.2"}}}}}};
        json siResp = rpc(cli, cfg.token, si);
        const json& sic = siResp["result"]["structuredContent"];
        check(sic.value("ok", false) && sic.contains("summary") && sic.value("bedDeclared", 0) == 10,
              "send_layout_inspect host shape (bedDeclared=10, summary present)");
    }

    // --- 2s. Batch T1 (per-object decode-coverage timeline): analysis.object_decode_timeline --------
    // The decode geometry is proven deterministically in unit.decode_timeline; here we assert presence
    // and that the explicit-trajectory path (which needs no REAPER) computes a real dominant-speaker
    // migration in the host build. Live panner reading is verified by scripts/verify_object_decode_timeline.py.
    {
        json req = {{"jsonrpc", "2.0"}, {"id", 970}, {"method", "tools/list"}, {"params", json::object()}};
        json listResp = rpc(cli, cfg.token, req);
        const json& list = listResp["result"]["tools"];
        auto listHas = [&](const char* nm) {
            for (const auto& t : list) if (t.value("name", "") == nm) return true;
            return false;
        };
        check(listHas("analysis.object_decode_timeline"),
              "Batch-T1 analysis.object_decode_timeline present in tools/list");

        json call = {{"jsonrpc", "2.0"}, {"id", 971}, {"method", "tools/call"},
                     {"params", {{"name", "analysis.object_decode_timeline"},
                                 {"arguments", {{"layout", "7.1.4"}, {"order", 3},
                                     {"trajectories", json::array({
                                         {{"name", "flyby"}, {"keyframes", json::array({
                                             {{"t", 0.0}, {"azimuthDeg", 90.0}},
                                             {{"t", 2.0}, {"azimuthDeg", 0.0}},
                                             {{"t", 4.0}, {"azimuthDeg", -90.0}}})}}})}}}}}};
        json callResp = rpc(cli, cfg.token, call);
        const json& sc = callResp["result"]["structuredContent"];
        check(sc.value("count", 0) == 1 && sc.contains("objects") && sc["objects"].is_array() &&
              sc["objects"].size() == 1 && sc.value("order", 0) == 3,
              "object_decode_timeline computes 1 explicit-trajectory object in the host build");
        bool migSpans = false, moving = false;
        if (sc["objects"].is_array() && !sc["objects"].empty()) {
            const json& o = sc["objects"][0];
            bool hasL = false, hasR = false;
            if (o["migration"].is_array())
                for (const auto& m : o["migration"]) {
                    const std::string s = m.get<std::string>();
                    if (s == "Lss") hasL = true;
                    if (s == "Rss") hasR = true;
                }
            migSpans = hasL && hasR;
            moving = (o["travel"].value("static", true) == false) &&
                     o["travel"].value("angularTravelDeg", 0.0) > 120.0;
        }
        check(migSpans, "flyby dominant speaker migrates across Lss..Rss");
        check(moving, "flyby is reported moving with ~180 deg of angular travel");
    }

    // --- teardown ---
    sseKeep.store(false);
    server.stop();
    check(!server.isRunning(), "server stops cleanly");
    if (sseThread.joinable()) sseThread.join();
    pump.store(false);
    mainThread.join();

    if (g_failures == 0) {
        std::fprintf(stderr, "ALL PROTOCOL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d PROTOCOL CHECK(S) FAILED\n", g_failures);
    return 1;
}
