// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tool_registry.h
//
// Registry for MCP tools. Tools declare:
//   - name, description, JSON-Schema input, structured-output schema
//   - MCP behavioral annotations (readOnlyHint / destructiveHint / idempotentHint)
//   - a capability "profile" tag so clients can negotiate a bounded tool set (LLM tool-count caps)
//   - a handler that runs ON THE MAIN THREAD (submitted via MainThreadQueue)
//
// Profiles: core, midi, mixing, routing, spatial, render, analysis, full.
// A small always-on meta-tool can enumerate and lazily surface additional tools on demand.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace reaper_mcp {

using Json = nlohmann::json;

// Parse a schema/object literal into a Json value. Tool definitions keep their schemas as readable
// raw-string JSON; jparse() turns them into real objects at registration (validated once, at load).
inline Json jparse(const char* s) { return Json::parse(s); }

enum class Profile { Core, Midi, Mixing, Routing, Spatial, Render, Analysis, Full };

// Profile <-> wire string (used by initialize negotiation and diagnostics).
inline const char* profileName(Profile p) {
    switch (p) {
        case Profile::Core:     return "core";
        case Profile::Midi:     return "midi";
        case Profile::Mixing:   return "mixing";
        case Profile::Routing:  return "routing";
        case Profile::Spatial:  return "spatial";
        case Profile::Render:   return "render";
        case Profile::Analysis: return "analysis";
        case Profile::Full:     return "full";
    }
    return "core";
}

inline bool parseProfile(const std::string& s, Profile& out) {
    if (s == "core") { out = Profile::Core; return true; }
    if (s == "midi") { out = Profile::Midi; return true; }
    if (s == "mixing") { out = Profile::Mixing; return true; }
    if (s == "routing") { out = Profile::Routing; return true; }
    if (s == "spatial") { out = Profile::Spatial; return true; }
    if (s == "render") { out = Profile::Render; return true; }
    if (s == "analysis") { out = Profile::Analysis; return true; }
    if (s == "full") { out = Profile::Full; return true; }
    return false;
}

struct ToolAnnotations {
    bool readOnly = false;
    bool destructive = false;
    bool idempotent = false;
};

struct Tool {
    std::string name;
    std::string description;
    Json inputSchema;   // JSON Schema for arguments
    Json outputSchema;  // JSON Schema for structured content
    ToolAnnotations annotations;
    Profile profile = Profile::Core;
    // Handler receives parsed args, returns structured result. Invoked on the MAIN THREAD.
    std::function<Json(const Json& args)> handler;
    // Always list this tool regardless of the session's negotiated profiles. Used for the
    // always-on meta-tool (tools.enumerate) so it stays discoverable even under a narrow profile.
    bool alwaysVisible = false;
};

class ToolRegistry {
public:
    void add(Tool t) { tools_[t.name] = std::move(t); }

    // tools/list, filtered by the session's negotiated profiles. Profile::Full matches everything.
    std::vector<const Tool*> list(const std::vector<Profile>& active) const {
        std::vector<const Tool*> out;
        for (auto& [name, tool] : tools_) {
            if (tool.alwaysVisible) { out.push_back(&tool); continue; }
            for (auto p : active) {
                if (p == Profile::Full || tool.profile == p) {
                    out.push_back(&tool);
                    break;
                }
            }
        }
        return out;
    }

    const Tool* find(const std::string& name) const {
        auto it = tools_.find(name);
        return it == tools_.end() ? nullptr : &it->second;
    }

    size_t size() const { return tools_.size(); }

private:
    std::map<std::string, Tool> tools_;
};

// Registration entry points implemented per translation unit (see src/tools/*.cpp).
void registerCoreTools(ToolRegistry&);      // transport / project / tracks
void registerMetaTools(ToolRegistry&);      // always-on tools.enumerate meta-tool
void registerMarkerTools(ToolRegistry&);    // markers / regions
void registerItemTools(ToolRegistry&);      // media items
void registerFxTools(ToolRegistry&);        // track FX add/list/param
void registerRoutingTools(ToolRegistry&);   // sends + track channel counts
void registerEnvelopeTools(ToolRegistry&);  // track envelopes / automation
void registerTimelineTools(ToolRegistry&);  // time selection + tempo
void registerMidiTools(ToolRegistry&);      // takes + MIDI note/CC CRUD
void registerSpatialTools(ToolRegistry&);   // spatial / immersive tools (ambisonics, beds, binaural monitor)
void registerAnalysisTools(ToolRegistry&);  // analysis.check_deliverable (deliverable specs)
void registerMixTools(ToolRegistry&);       // mix.apply_style (immersive-aware style chains)
void registerSessionTools(ToolRegistry&);   // session.run_dsl (deterministic macro-DSL runner)
void registerActionTools(ToolRegistry&);    // action.run / run_by_name / get_toggle_state

}  // namespace reaper_mcp
