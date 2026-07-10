// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_meta.cpp — the always-on meta-tool for tool-count management.
//
// Some MCP clients cap the number of tools they will surface (~128). Our answer: negotiate a small
// profile set at initialize (e.g. {"profiles":["core"]}), then let the agent DISCOVER the rest with
// this always-visible tool and CALL them by name — tools/call dispatches any registered tool
// regardless of the session's active profiles, so a tool that never appeared in tools/list is still
// invocable once discovered here.
//
// This is not a REAPER tool: it reads the registry (immutable after startup) and touches no API, so
// it is identical in the native and host builds.

#include <utility>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

void registerMetaTools(ToolRegistry& reg) {
    ToolRegistry* self = &reg;  // enumerated at call time; the registry outlives every tool

    Tool t{
        "tools.enumerate",
        "Meta-tool: list every available tool, INCLUDING tools outside the session's negotiated "
        "profile set, optionally filtered by profile or a name/description substring. Any tool listed "
        "here can be invoked by name via tools/call even if it did not appear in tools/list. Use this "
        "to stay under a client's tool-count cap: negotiate a small profile at initialize, then "
        "discover and call the rest on demand. Pass detail=true to also get each tool's JSON schemas.",
        jparse(R"({"type":"object","properties":{
            "profile":{"type":"string",
                "enum":["core","midi","mixing","routing","spatial","render","analysis","full","all"],
                "description":"restrict to one capability profile; 'all'/'full' = no filter"},
            "query":{"type":"string",
                "description":"case-insensitive substring matched against tool name + description"},
            "detail":{"type":"boolean","default":false,
                "description":"include each tool's full input/output JSON schemas"}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "count":{"type":"integer"},"totalRegistered":{"type":"integer"},
            "tools":{"type":"array","items":{"type":"object","properties":{
                "name":{"type":"string"},"description":{"type":"string"},"profile":{"type":"string"},
                "readOnly":{"type":"boolean"},"destructive":{"type":"boolean"},
                "idempotent":{"type":"boolean"}}}}},
            "required":["count","totalRegistered","tools"]})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Core,
        [self](const Json& a) -> Json {
            const std::string wantProfile = optStr(a, "profile", "all");
            const std::string query = toLower(optStr(a, "query", ""));
            const bool detail = optBool(a, "detail", false);

            Profile pf = Profile::Core;
            const bool haveProfileFilter =
                (wantProfile != "all" && wantProfile != "full") && parseProfile(wantProfile, pf);

            // list({Full}) returns every registered tool, independent of the session's profiles.
            const auto all = self->list({Profile::Full});
            Json tools = Json::array();
            for (const Tool* t : all) {
                if (haveProfileFilter && t->profile != pf) continue;
                if (!query.empty()) {
                    if (toLower(t->name + " " + t->description).find(query) == std::string::npos)
                        continue;
                }
                Json j{{"name", t->name},
                       {"description", t->description},
                       {"profile", profileName(t->profile)},
                       {"readOnly", t->annotations.readOnly},
                       {"destructive", t->annotations.destructive},
                       {"idempotent", t->annotations.idempotent}};
                if (detail) {
                    j["inputSchema"] = t->inputSchema;
                    if (!t->outputSchema.is_null()) j["outputSchema"] = t->outputSchema;
                }
                tools.push_back(std::move(j));
            }
            return Json{{"count", static_cast<int>(tools.size())},
                        {"totalRegistered", static_cast<int>(self->size())},
                        {"tools", std::move(tools)}};
        }};
    t.alwaysVisible = true;  // discoverable under any negotiated profile
    reg.add(std::move(t));
}

}  // namespace reaper_mcp
