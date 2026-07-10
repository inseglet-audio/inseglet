// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// reaper_mcp.cpp
//
// REAPER extension entry point. When REAPER starts it dlopen/LoadLibrary's this binary and calls
// ReaperPluginEntry(hInstance, rec). We:
//   1. Verify the caller version and import the REAPER API via rec->GetFunc (REAPERAPI_LoadAPI).
//   2. Register an action ("reaper_mcp: status") so the user can confirm the extension loaded and
//      see the server URL/token.
//   3. Register our control surface (csurf_inst) — its Run() is our MAIN-THREAD PUMP and its Set*
//      callbacks are our push-event source. Also register a "timer" as a belt-and-suspenders pump.
//   4. Stand up the tool registry and start the in-process MCP server (worker thread).
//   5. On unload (rec == NULL) tear everything down (unregister first, then destroy).

// The ONE translation unit that instantiates the imported REAPER function pointers.
#ifdef REAPER_MCP_HAVE_SDK
  #define REAPERAPI_IMPLEMENT
#endif
#include "reaper_api.h"  // SDK headers + WANT set (no-op without REAPER_MCP_HAVE_SDK)

#ifndef REAPER_MCP_HAVE_SDK
  // Minimal shims so the scaffold is self-documenting and compiles for structural review (no SDK).
  struct HINSTANCE__; using HINSTANCE = HINSTANCE__*;
  struct reaper_plugin_info_t {
      int caller_version;
      void* hwnd_main;
      int (*Register)(const char* name, void* infostruct);
      void* (*GetFunc)(const char* name);
  };
  #define REAPER_PLUGIN_VERSION 0x20E
#endif

#include <memory>
#include <random>
#include <string>
#if defined(_WIN32)
  #include <process.h>
#else
  #include <unistd.h>
#endif

#include "control_surface.h"
#include "discovery.h"
#include "main_thread_queue.h"
#include "mcp_server.h"
#include "prompt_registry.h"
#include "resource_registry.h"
#include "subscription_hub.h"
#include "tool_registry.h"

#include <vector>

#if defined(_WIN32)
  #define REAPER_MCP_EXPORT extern "C" __declspec(dllexport)
#else
  #define REAPER_MCP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace reaper_mcp {
namespace {

std::unique_ptr<MainThreadQueue> g_queue;
std::unique_ptr<ToolRegistry> g_tools;
std::unique_ptr<ResourceRegistry> g_resources;
std::unique_ptr<PromptRegistry> g_prompts;
std::unique_ptr<SubscriptionHub> g_subs;
std::unique_ptr<McpServer> g_server;
std::unique_ptr<McpControlSurface> g_surface;

// Map a control-surface StateEvent to the resource URI(s) it invalidates. Runs on the MAIN THREAD
// (called from the Set* callbacks), so resolving a MediaTrack* to its index via the REAPER API is
// safe here. Over-notifying is harmless — the client just re-reads a resource that may be unchanged —
// so track-scoped edits conservatively invalidate the whole project snapshot plus that track's chunk.
std::vector<std::string> urisForEvent(const StateEvent& e) {
    std::vector<std::string> uris;
    switch (e.kind) {
        case StateEvent::Kind::TrackList:
            // Add/remove/reorder a track (or its sends) changes both the snapshot and the topology.
            uris.push_back("reaper://project/state");
            uris.push_back("reaper://routing/graph");
            break;
        case StateEvent::Kind::PlayState:
            uris.push_back("reaper://project/state");
            break;
        default:  // Volume / Pan / Mute / Solo / RecArm / Selection / TrackTitle / Other (track-scoped)
            uris.push_back("reaper://project/state");
#ifdef REAPER_MCP_HAVE_SDK
            if (e.track) {
                const int idx = CSurf_TrackToID(e.track, false) - 1;  // 1-based incl. master; -1 => master
                if (idx >= 0) uris.push_back("reaper://track/" + std::to_string(idx) + "/chunk");
            }
#endif
            break;
    }
    return uris;
}

int (*g_reg)(const char*, void*) = nullptr;  // saved rec->Register for unregistration at unload
std::string g_discoveryPath;                 // reaper_mcp.json path (written on start, removed on unload)

std::string makeToken() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd());
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i) s.push_back(hex[dist(gen)]);
    return s;
}

// The main-thread fallback pump (registered as "timer"). Complements McpControlSurface::Run().
void onTimerPump() {
    if (g_queue) g_queue->drainMainThread();
}

#ifdef REAPER_MCP_HAVE_SDK
int g_cmd = 0;
gaccel_register_t g_gaccel{};

long currentPid() {
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// Write {url, token, sessionId, pid, ...} to a stable path so clients don't hand-copy the
// per-launch port/token. Called after the server binds.
void writeDiscovery() {
    if (!g_server) return;
    g_discoveryPath = std::string(GetResourcePath()) + "/reaper_mcp.json";
    const std::string url =
        "http://127.0.0.1:" + std::to_string(g_server->boundPort()) + "/mcp";
    Json info = {{"url", url},
                 {"host", "127.0.0.1"},
                 {"port", g_server->boundPort()},
                 {"token", g_server->token()},
                 {"sessionId", g_server->sessionId()},
                 {"authHeader", "Authorization: Bearer " + g_server->token()},
                 {"protocol", "2025-06-18"},
                 {"server", "reaper_mcp"},
                 {"version", "1.5.0"},
                 {"pid", currentPid()}};
    writeDiscoveryFile(g_discoveryPath, info);
}

void showStatus() {
    if (!g_server) return;
    std::string m = "reaper_mcp: MCP server on http://127.0.0.1:" +
                    std::to_string(g_server->boundPort()) + "/mcp\n" +
                    "  Authorization: Bearer " + g_server->token() + "\n" +
                    "  Mcp-Session-Id: " + g_server->sessionId() + "\n" +
                    "  discovery file: " + g_discoveryPath + "\n" +
                    "  queue pending=" + std::to_string(g_queue ? g_queue->pending() : 0) +
                    "   (Run() + timer pump active)\n";
    ShowConsoleMsg(m.c_str());
}

bool onCommand(int command, int /*flag*/) {
    if (g_cmd && command == g_cmd) {
        showStatus();
        return true;  // handled
    }
    return false;
}
#endif  // REAPER_MCP_HAVE_SDK

void startUp(reaper_plugin_info_t* rec) {
    g_reg = rec->Register;

    g_queue = std::make_unique<MainThreadQueue>();
    g_tools = std::make_unique<ToolRegistry>();

    registerCoreTools(*g_tools);       // transport / project / tracks
    registerMetaTools(*g_tools);       // always-on tools.enumerate meta-tool
    registerMarkerTools(*g_tools);     // markers / regions
    registerItemTools(*g_tools);       // media items
    registerFxTools(*g_tools);         // track FX add/list/param
    registerRoutingTools(*g_tools);    // sends + channel counts
    registerEnvelopeTools(*g_tools);   // envelopes / automation
    registerTimelineTools(*g_tools);   // time selection + tempo
    registerMidiTools(*g_tools);       // takes + MIDI note/CC CRUD
    registerSpatialTools(*g_tools);    // spatial / immersive tools (ambisonics, beds, binaural monitor)
    registerAnalysisTools(*g_tools);   // analysis.check_deliverable (deliverable-spec conformance)
    registerMixTools(*g_tools);        // mix.apply_style (immersive-aware style chains)
    registerSessionTools(*g_tools);    // session.run_dsl (deterministic macro-DSL over the verbs)
    registerActionTools(*g_tools);     // action.run / run_by_name / get_toggle_state

    // Resource providers (.RPP chunk / routing graph) — read on the main thread via the queue.
    g_resources = std::make_unique<ResourceRegistry>();
    registerProjectResources(*g_resources);

    // Prompts: expert immersive workflows surfaced in the client's prompt picker. Pure
    // data — rendered on the request thread, no main-thread hop.
    g_prompts = std::make_unique<PromptRegistry>();
    registerExpertPrompts(*g_prompts);

    // Subscription hub: bridges the control surface's push events to the SSE channel.
    g_subs = std::make_unique<SubscriptionHub>();

    // Control surface = main-thread pump + push events. The sink runs on the MAIN THREAD (from the
    // Set* callbacks): it maps each change to the affected resource URI(s) and marks them dirty; the
    // hub only records URIs a client actually subscribed to, and the SSE channel flushes the rest.
    SubscriptionHub* subs = g_subs.get();
    g_surface = std::make_unique<McpControlSurface>(*g_queue, [subs](const StateEvent& e) {
        for (const std::string& uri : urisForEvent(e)) subs->markDirty(uri);
    });

#ifdef REAPER_MCP_HAVE_SDK
    // Smoke action so the user can confirm load + see the endpoint/token.
    g_cmd = g_reg("command_id", (void*)"REAPERMCPSTATUS");
    g_gaccel.accel.cmd = static_cast<unsigned short>(g_cmd);
    g_gaccel.desc = "reaper_mcp: status";
    g_reg("gaccel", &g_gaccel);
    g_reg("hookcommand", (void*)&onCommand);

    // Register the control surface INSTANCE so Run()/Set* fire without the user configuring anything.
    g_reg("csurf_inst", (void*)static_cast<IReaperControlSurface*>(g_surface.get()));
    // Belt-and-suspenders main-thread pump.
    g_reg("timer", (void*)&onTimerPump);
#endif

    ServerConfig cfg;
    cfg.token = makeToken();
    g_server = std::make_unique<McpServer>(cfg, *g_tools, *g_resources, *g_prompts, *g_subs, *g_queue);
    g_server->start();

#ifdef REAPER_MCP_HAVE_SDK
    writeDiscovery();
    showStatus();
#endif
}

void shutDown() {
#ifdef REAPER_MCP_HAVE_SDK
    if (g_reg) {
        g_reg("-timer", (void*)&onTimerPump);
        if (g_surface) g_reg("-csurf_inst", (void*)static_cast<IReaperControlSurface*>(g_surface.get()));
        g_reg("-hookcommand", (void*)&onCommand);
        g_reg("-gaccel", &g_gaccel);
    }
    g_cmd = 0;
    if (!g_discoveryPath.empty()) {
        removeDiscoveryFile(g_discoveryPath);
        g_discoveryPath.clear();
    }
#endif
    // Close the head-tracking OSC listener (socket + worker thread) before tearing the rest
    // down. Safe/no-op if it was never started. Done after unregistering csurf_inst so Run() (which
    // calls applyOnMainThread) is no longer firing.
    HeadTrackBridge::instance().stop();
    if (g_server) g_server->stop();
    g_server.reset();
    g_surface.reset();  // destroy AFTER unregistering csurf_inst
    g_subs.reset();     // after the server (references it) and the surface (sink captures it)
    g_resources.reset();
    g_prompts.reset();  // after the server (references it)
    g_tools.reset();
    g_queue.reset();
    g_reg = nullptr;
}

}  // namespace
}  // namespace reaper_mcp

REAPER_MCP_EXPORT int ReaperPluginEntry(HINSTANCE /*hInstance*/, reaper_plugin_info_t* rec) {
    using namespace reaper_mcp;
    if (!rec) {  // unload
        shutDown();
        return 0;
    }
    if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) {
        return 0;  // incompatible host
    }
#ifdef REAPER_MCP_HAVE_SDK
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        return 0;  // a required (WANTed) API function was missing
    }
#endif
    startUp(rec);
    return 1;  // stay resident
}
