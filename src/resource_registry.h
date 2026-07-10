// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// resource_registry.h
//
// Registry for MCP *resources* — read-only, addressable snapshots of REAPER state that a client can
// list and fetch by URI (MCP 2025-06-18 resources/list, resources/read, resources/templates/list).
//
// Two flavors:
//   - static resource:   a fixed URI (e.g. reaper://project/state, reaper://routing/graph)
//   - resource template: a URI with {placeholders} (e.g. reaper://track/{index}/chunk), matched at
//                         read time; the reader parses the concrete values out of the requested URI.
//
// A resource's read() runs ON THE MAIN THREAD (submitted via MainThreadQueue by the server), exactly
// like a tool handler, because it touches the REAPER API. It returns the text payload; the declared
// mimeType tells the client how to interpret it (application/json for our derived snapshots).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace reaper_mcp {

using Json = nlohmann::json;

struct Resource {
    std::string uri;          // exact URI, or the template string if isTemplate
    std::string name;
    std::string description;
    std::string mimeType = "application/json";
    bool isTemplate = false;  // uri contains {param} placeholders
    // Runs on the MAIN THREAD. Receives the concrete requested URI (== uri for static resources).
    // Returns the resource contents as text. Throws std::runtime_error on bad input.
    std::function<std::string(const std::string& uri)> read;
};

class ResourceRegistry {
public:
    void add(Resource r) { resources_.push_back(std::move(r)); }

    const std::vector<Resource>& all() const { return resources_; }

    // Resolve a requested URI to a static resource (exact match) or a template (pattern match).
    const Resource* resolve(const std::string& uri) const {
        for (const auto& r : resources_)
            if (!r.isTemplate && r.uri == uri) return &r;
        for (const auto& r : resources_)
            if (r.isTemplate && templateMatches(r.uri, uri)) return &r;
        return nullptr;
    }

    // Loose match of a {placeholder} template against a concrete URI: the literal text before the
    // first '{' and after the last '}' must bracket a non-empty middle. The reader does exact parsing.
    static bool templateMatches(const std::string& tmpl, const std::string& uri) {
        const auto open = tmpl.find('{');
        const auto close = tmpl.rfind('}');
        if (open == std::string::npos || close == std::string::npos || close < open) return tmpl == uri;
        const std::string prefix = tmpl.substr(0, open);
        const std::string suffix = tmpl.substr(close + 1);
        if (uri.size() <= prefix.size() + suffix.size()) return false;
        if (uri.compare(0, prefix.size(), prefix) != 0) return false;
        if (uri.compare(uri.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        return true;
    }

private:
    std::vector<Resource> resources_;
};

// Registration entry point (see src/resources/resources_project.cpp).
void registerProjectResources(ResourceRegistry&);  // project state / routing graph / RPP

}  // namespace reaper_mcp
