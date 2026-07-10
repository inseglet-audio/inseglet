// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// prompts_expert.cpp — the 3 expert-workflow MCP prompts.
//
// Each prompt is a declarative, client-visible template that steers an agent to drive the
// semantic verbs in the right order for a common immersive outcome, so a non-expert user gets an
// expert workflow from the client's prompt picker without knowing the verb names:
//
//   setup_atmos_session   -> spatial.setup_immersive_session   (bed + objects + binaural monitor +
//                                                                Dolby Renderer sends)
//   encode_to_ambisonics  -> spatial.stereo_to_ambisonic / spatial.spatialize_stems
//   master_for_delivery   -> mix.apply_style + analysis.check_deliverable  (loudness/TP conformance)
//
// Pure data + deterministic render(); no REAPER API. Every verb these templates steer toward is
// implemented, and the whole flow is reproducible as a session.run_dsl macro. The loudness/true-peak
// figures below are the deliverable specs.

#include <string>
#include <vector>

#include "../prompt_registry.h"

namespace reaper_mcp {
namespace {

// prompts/get argument values are strings by the MCP spec. Read one with a documented default.
std::string argStr(const Json& args, const char* key, const std::string& def) {
    if (args.is_object() && args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return def;
}

std::vector<PromptMessage> renderSetupAtmos(const Json& args) {
    const std::string bed = argStr(args, "beds", "7.1.4");
    const std::string objects = argStr(args, "objects", "0");
    const std::string monitor = argStr(args, "monitor", "binaural");
    const bool wantMonitor = monitor != "none" && monitor != "false";

    std::string t;
    t += "Set up an immersive Dolby Atmos session in REAPER.\n\n";
    t += "Target: a " + bed + " bed";
    if (objects != "0" && !objects.empty()) t += " plus " + objects + " object track(s)";
    t += wantMonitor ? ", with a binaural monitor for auditioning.\n\n" : ".\n\n";
    t += "Do it in one composite call:\n";
    t += "1. Call spatial.detect_spatial_suites first to confirm an ambisonic/binaural suite (IEM > "
         "SPARTA > ATK) is available for the monitor; if none is, report that and offer to proceed "
         "without the binaural monitor.\n";
    t += "2. Call spatial.setup_immersive_session with { \"bed\": \"" + bed + "\", \"objectCount\": " +
         (objects.empty() ? "0" : objects) + ", \"monitor\": " + (wantMonitor ? "true" : "false") +
         ", \"rendererSends\": true }. This builds the bed bus, tags the object tracks, adds the "
         "binaural monitor, and lays out the external Dolby Renderer / DAPS sends (REAPER does not "
         "author ADM natively — we orchestrate the send matrix).\n";
    t += "3. This verb is destructive if the project is not empty. First call it with "
         "\"dryRun\": true and show me the plan/diff; only after I approve, re-issue it with "
         "\"confirm\": true.\n";
    t += "4. Verify the result against the reaper://routing/graph resource: the bed bus has the right "
         "channel count (" + bed + " => 12ch for 7.1.4, 16ch for 9.1.6), each object routes to the "
         "renderer bus, and the monitor bus is present and untouched-source.\n";
    return {PromptMessage{"user", t}};
}

std::vector<PromptMessage> renderEncodeAmbisonics(const Json& args) {
    const std::string src = argStr(args, "sourceBus", "the selected track");
    const std::string order = argStr(args, "order", "1");
    const std::string norm = argStr(args, "normalization", "SN3D");
    const std::string monitor = argStr(args, "monitor", "binaural");
    const bool wantMonitor = monitor != "none" && monitor != "false";

    std::string t;
    t += "Encode " + src + " to an order-" + order + " ambisonic scene in REAPER.\n\n";
    t += "1. Call spatial.detect_spatial_suites to pick the encoder (IEM > SPARTA > ATK).\n";
    t += "2. For a mono/stereo source, call spatial.stereo_to_ambisonic with { \"source\": \"" + src +
         "\", \"order\": " + order + ", \"normalization\": \"" + norm + "\"" +
         (wantMonitor ? ", \"monitor\": true" : "") +
         " }. For multiple stems that each need their own azimuth/elevation, use "
         "spatial.spatialize_stems with an ambisonic target { \"ambisonicOrder\": " + order +
         ", \"normalization\": \"" + norm + "\" } instead.\n";
    t += "3. Confirm the report: the track has (order+1)^2 channels (order " + order + " => " +
         (order == "1" ? "4" : (order == "2" ? "9 (padded to 10)" : (order == "3" ? "16" : "(order+1)^2"))) +
         "ch), channel ordering is ACN, normalization is " + norm +
         ", and the encoder's output pins are identity-wired onto the HOA channels.\n";
    t += "4. stereo_to_ambisonic is an upmix *sketch*, not a true soundfield capture — say so when you "
         "report back.\n";
    return {PromptMessage{"user", t}};
}

std::vector<PromptMessage> renderMasterForDelivery(const Json& args) {
    const std::string spec = argStr(args, "spec", "atmos-music");
    const std::string target = argStr(args, "target", "the master");

    std::string t;
    t += "Master " + target + " to the \"" + spec + "\" deliverable spec and tell me whether it "
         "passes.\n\n";
    t += "Reference targets (integrated LUFS / true peak): atmos-music -18 / -1 dBTP (7.1.4); "
         "streaming-stereo -14 / -1; apple-music-stereo -16 / -1; ebu-r128 -23 (+/-0.5 LU) / -1; "
         "ebu-r128-s1 -23, Max-ST <= -18 / -1; atsc-a85 -24 LKFS (+/-2 LK) / -2 dBTP; podcast -16 "
         "(-19 mono) / -1; cinema-theatrical SPL-referenced (not LUFS-gated) / -3. Measurement is "
         "ITU-R BS.1770-5 (K-weighted LUFS + inter-sample true peak), EBU Tech 3341 metering.\n\n";
    t += "1. Call mix.apply_style with { \"track\": \"" + target + "\", \"targetSpec\": \"" + spec +
         "\" } choosing an immersive-aware style whose limiter ceiling matches the spec's true peak "
         "and that never routes/limits the LFE with the mains. Use \"dryRun\": true first and show me "
         "the chain.\n";
    t += "2. Call analysis.check_deliverable with { \"target\": \"" + target + "\", \"spec\": \"" +
         spec + "\", \"perBed\": true } to measure integrated LUFS, momentary/short-term max, and "
         "true-peak (dBTP), plus per-bed loudness for immersive masters. This is non-destructive "
         "(report, don't ship).\n";
    t += "3. If any metric fails, adjust the style (gain/limiter) and re-check until it passes. For "
         "cinema-theatrical, report true-peak plus the SPL-referenced note rather than a LUFS "
         "pass/fail.\n";
    t += "4. To make this reproducible and single-undoable, you can run the whole apply_style -> "
         "check_deliverable loop as one session.run_dsl macro, e.g. a script of "
         "'apply_style track=" + target + " style=<name> spec=" + spec +
         "' then 'check spec=" + spec + " target=master' — deterministic, one undo point.\n";
    return {PromptMessage{"user", t}};
}

}  // namespace

void registerExpertPrompts(PromptRegistry& reg) {
    reg.add(Prompt{
        "setup_atmos_session",
        "Set up an Atmos bed + objects session",
        "Scaffold an immersive Dolby Atmos session — a bed, N object tracks, a binaural monitor, and "
        "the external Dolby Renderer/DAPS send layout — via spatial.setup_immersive_session.",
        {PromptArg{"beds", "Bed layout, e.g. 7.1.4 or 9.1.6 (default 7.1.4)", false},
         PromptArg{"objects", "Number of object tracks to create (default 0)", false},
         PromptArg{"monitor", "Binaural monitor: 'binaural' or 'none' (default binaural)", false}},
        renderSetupAtmos});

    reg.add(Prompt{
        "encode_to_ambisonics",
        "Encode a source to an ambisonic scene",
        "Encode a mono/stereo bus (or spatialize multiple stems) into an order-N ambisonic scene "
        "(ACN/SN3D) via spatial.stereo_to_ambisonic / spatial.spatialize_stems.",
        {PromptArg{"sourceBus", "Track/bus to encode (name or index)", true},
         PromptArg{"order", "Ambisonic order, e.g. 1 or 3 (default 1)", false},
         PromptArg{"normalization", "SN3D or FuMa (default SN3D)", false},
         PromptArg{"monitor", "Binaural monitor: 'binaural' or 'none' (default binaural)", false}},
        renderEncodeAmbisonics});

    reg.add(Prompt{
        "master_for_delivery",
        "Master to a deliverable spec",
        "Apply an immersive-aware master chain and check the result against a named loudness/true-peak "
        "deliverable spec (atmos-music, atsc-a85, ebu-r128, streaming-stereo, …) via mix.apply_style + "
        "analysis.check_deliverable.",
        {PromptArg{"spec", "Deliverable spec name, e.g. atmos-music, atsc-a85, streaming-stereo", true},
         PromptArg{"target", "Track/bus to master (default: the master)", false}},
        renderMasterForDelivery});
}

}  // namespace reaper_mcp
