// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// prompt_registry.h
//
// Registry for MCP *prompts* — declarative, client-visible expert workflows (MCP 2025-06-18
// prompts/list + prompts/get). A prompt is a named, parameterized message template that shows up in
// the client's prompt picker; selecting one injects a message that steers the agent to call the
// semantic verbs in the right order. This is how a non-expert user gets an expert immersive
// workflow without knowing the verb names.
//
// Prompts are pure data + a deterministic render() — no REAPER API, no threads. Like tool_registry.h
// this header pulls only nlohmann/json, so it is safe to include from the (dep-free) server header.
//
// prompts/get argument values are strings by the MCP spec; render() reads them out of a JSON object
// of {name: "value"} and substitutes them into the message text, applying documented defaults for
// optional arguments.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace reaper_mcp {

using Json = nlohmann::json;

// A single declared argument of a prompt (advertised in prompts/list so the client can collect it).
struct PromptArg {
    std::string name;
    std::string description;
    bool required = false;
};

// One rendered message. content.type == "text" on the wire; role is "user" or "assistant".
struct PromptMessage {
    std::string role;
    std::string text;
};

struct Prompt {
    std::string name;
    std::string title;        // human-facing label (optional; falls back to name in list output)
    std::string description;
    std::vector<PromptArg> arguments;
    // Render the message list from the (already required-validated) argument object. Deterministic.
    std::function<std::vector<PromptMessage>(const Json& args)> render;
};

class PromptRegistry {
public:
    void add(Prompt p) { prompts_[p.name] = std::move(p); }

    const Prompt* find(const std::string& name) const {
        auto it = prompts_.find(name);
        return it == prompts_.end() ? nullptr : &it->second;
    }

    size_t size() const { return prompts_.size(); }

    // prompts/list payload: {"prompts":[{name,title?,description,arguments:[{name,description,required}]}]}
    // Deterministic order (std::map is sorted by name), so client pickers and tests are stable.
    Json listJson() const {
        Json arr = Json::array();
        for (const auto& [name, p] : prompts_) {
            Json args = Json::array();
            for (const auto& a : p.arguments)
                args.push_back(Json{{"name", a.name},
                                    {"description", a.description},
                                    {"required", a.required}});
            Json j{{"name", p.name}, {"description", p.description}, {"arguments", std::move(args)}};
            if (!p.title.empty()) j["title"] = p.title;
            arr.push_back(std::move(j));
        }
        return Json{{"prompts", std::move(arr)}};
    }

    // prompts/get payload for a resolved prompt with validated arguments:
    // {"description":..., "messages":[{"role":..., "content":{"type":"text","text":...}}]}
    static Json getJson(const Prompt& p, const Json& args) {
        Json messages = Json::array();
        for (const auto& m : p.render(args))
            messages.push_back(Json{{"role", m.role},
                                    {"content", {{"type", "text"}, {"text", m.text}}}});
        return Json{{"description", p.description}, {"messages", std::move(messages)}};
    }

private:
    std::map<std::string, Prompt> prompts_;
};

// Registration entry point (see src/prompts/prompts_expert.cpp) — the 3 expert workflows:
// setup_atmos_session, encode_to_ambisonics, master_for_delivery.
void registerExpertPrompts(PromptRegistry&);

}  // namespace reaper_mcp
