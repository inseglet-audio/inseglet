// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// docs/gen/dump_reference.cpp
//
// Dev tool — NOT shipped, NOT part of a normal build (EXCLUDE_FROM_ALL). Built + run by the
// `reference-doc` CMake target, which feeds its JSON to docs/gen/generate_reference.py to
// (re)generate docs/REFERENCE.md.
//
// It links `reaper_mcp_hostcore` and assembles the tool/resource/prompt registry with the SAME
// registration calls, in the SAME order, as the host protocol test (tests/unit/test_mcp_server.cpp),
// minus that test's harness-only `test.elicit_confirm` verb — so the dump is exactly the production
// surface the running server serves over tools/list + resources/list + prompts/list.
//
// Output: pretty JSON written to argv[1] if given, else stdout.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "tool_registry.h"
#include "resource_registry.h"
#include "prompt_registry.h"

using namespace reaper_mcp;

int main(int argc, char** argv) {
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
    registerAnalysisTools(tools);
    registerMixTools(tools);
    registerSessionTools(tools);
    registerActionTools(tools);     // Phase 8 P8-2

    ResourceRegistry resources;
    registerProjectResources(resources);

    PromptRegistry prompts;
    registerExpertPrompts(prompts);

    Json tarr = Json::array();
    for (const Tool* t : tools.list({Profile::Full})) {
        Json j{
            {"name", t->name},
            {"description", t->description},
            {"profile", profileName(t->profile)},
            {"annotations", {{"readOnlyHint", t->annotations.readOnly},
                             {"destructiveHint", t->annotations.destructive},
                             {"idempotentHint", t->annotations.idempotent}}},
            {"inputSchema", t->inputSchema},
            {"alwaysVisible", t->alwaysVisible},
        };
        if (!t->outputSchema.is_null()) j["outputSchema"] = t->outputSchema;
        tarr.push_back(std::move(j));
    }

    Json rarr = Json::array();
    for (const auto& r : resources.all()) {
        rarr.push_back({{"uri", r.uri}, {"name", r.name}, {"description", r.description},
                        {"mimeType", r.mimeType}, {"isTemplate", r.isTemplate}});
    }

    Json out{
        {"tools", std::move(tarr)},
        {"toolCount", static_cast<int>(tools.size())},
        {"resources", std::move(rarr)},
        {"prompts", prompts.listJson()["prompts"]},
    };

    const std::string text = out.dump(2);
    if (argc > 1) {
        std::ofstream f(argv[1]);
        if (!f) {
            std::fprintf(stderr, "dump_reference: cannot open '%s' for writing\n", argv[1]);
            return 1;
        }
        f << text << "\n";
    } else {
        std::cout << text << "\n";
    }
    return 0;
}
