// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_prompts.cpp — pure unit test for the Phase-5a PromptRegistry + the expert-workflow templates.
//
// No REAPER, no HTTP: exercises registerExpertPrompts() and the registry's listJson()/getJson()
// directly. Verifies the prompts/list + prompts/get payload shapes (MCP 2025-06-18), deterministic
// ordering, declared required arguments, and that render() substitutes arguments / applies defaults
// and steers toward the right Phase-5 verbs.

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "prompt_registry.h"

using nlohmann::json;
using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Mirror the server's prompts/get required-argument validation so we test the same contract.
static bool requiredArgsOk(const Prompt& p, const json& args) {
    for (const auto& a : p.arguments)
        if (a.required && (!args.contains(a.name) || !args[a.name].is_string() ||
                           args[a.name].get<std::string>().empty()))
            return false;
    return true;
}

int main() {
    PromptRegistry reg;
    registerExpertPrompts(reg);

    check(reg.size() == 3, "registerExpertPrompts registers 3 prompts");

    // ---- listJson: shape + deterministic order ----
    const json list = reg.listJson();
    check(list.contains("prompts") && list["prompts"].is_array(), "listJson has prompts[]");
    const json& ps = list["prompts"];
    check(ps.size() == 3, "listJson lists all 3");
    check(ps[0].value("name", "") == "encode_to_ambisonics", "order[0] = encode_to_ambisonics");
    check(ps[1].value("name", "") == "master_for_delivery", "order[1] = master_for_delivery");
    check(ps[2].value("name", "") == "setup_atmos_session", "order[2] = setup_atmos_session");
    for (const auto& p : ps) {
        check(p.contains("description") && !p.value("description", "").empty(), "prompt has description");
        check(p.contains("title") && !p.value("title", "").empty(), "prompt has title");
        check(p["arguments"].is_array(), "prompt has arguments[]");
        for (const auto& a : p["arguments"])
            check(a.contains("name") && a.contains("description") && a.contains("required"),
                  "argument has name+description+required");
    }

    // ---- required arguments ----
    const Prompt* enc = reg.find("encode_to_ambisonics");
    const Prompt* master = reg.find("master_for_delivery");
    const Prompt* setup = reg.find("setup_atmos_session");
    check(enc && master && setup, "all three prompts are findable by name");
    check(!requiredArgsOk(*enc, json::object()), "encode_to_ambisonics requires sourceBus");
    check(requiredArgsOk(*enc, json{{"sourceBus", "Music Bus"}}), "encode_to_ambisonics ok with sourceBus");
    check(!requiredArgsOk(*master, json::object()), "master_for_delivery requires spec");

    // ---- getJson: message shape + argument substitution ----
    {
        const json g = PromptRegistry::getJson(*setup, json{{"beds", "9.1.6"}, {"objects", "6"}});
        check(g.contains("description") && g["messages"].is_array(), "getJson has description+messages[]");
        check(!g["messages"].empty(), "getJson renders at least one message");
        const json& m0 = g["messages"][0];
        check(m0.value("role", "") == "user", "message role is user");
        check(m0["content"].value("type", "") == "text", "message content type is text");
        const std::string text = m0["content"].value("text", "");
        check(text.find("9.1.6") != std::string::npos, "substitutes the bed layout (9.1.6)");
        check(text.find("6 object") != std::string::npos, "substitutes the object count (6)");
        check(text.find("setup_immersive_session") != std::string::npos,
              "steers toward spatial.setup_immersive_session");
        check(text.find("confirm") != std::string::npos, "mentions the confirm:true destructive gate");
    }

    // ---- getJson: optional-argument defaults ----
    {
        const json g = PromptRegistry::getJson(*setup, json::object());
        const std::string text = g["messages"][0]["content"].value("text", "");
        check(text.find("7.1.4") != std::string::npos, "default bed layout is 7.1.4 when omitted");
    }
    {
        const json g = PromptRegistry::getJson(*enc, json{{"sourceBus", "Vox"}, {"order", "3"}});
        const std::string text = g["messages"][0]["content"].value("text", "");
        check(text.find("Vox") != std::string::npos, "encode substitutes the source bus name");
        check(text.find("16") != std::string::npos, "order-3 ambisonics reports 16 channels ((3+1)^2)");
        check(text.find("stereo_to_ambisonic") != std::string::npos, "steers toward stereo_to_ambisonic");
    }
    {
        const json g = PromptRegistry::getJson(*master, json{{"spec", "atsc-a85"}});
        const std::string text = g["messages"][0]["content"].value("text", "");
        check(text.find("atsc-a85") != std::string::npos, "master substitutes the spec name");
        check(text.find("check_deliverable") != std::string::npos, "steers toward analysis.check_deliverable");
        check(text.find("apply_style") != std::string::npos, "steers toward mix.apply_style");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL PROMPT CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d PROMPT CHECK(S) FAILED\n", g_failures);
    return 1;
}
