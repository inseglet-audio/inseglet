// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// prompts_expert.cpp — the 5 expert-workflow MCP prompts.
//
// Each prompt is a declarative, client-visible template that steers an agent to drive the
// semantic verbs in the right order for a common immersive outcome, so a non-expert user gets an
// expert workflow from the client's prompt picker without knowing the verb names:
//
//   setup_atmos_session   -> spatial.setup_immersive_session   (bed + objects + binaural monitor +
//                                                                Dolby Renderer sends)
//   encode_to_ambisonics  -> spatial.stereo_to_ambisonic / spatial.spatialize_stems
//   master_for_delivery   -> mix.apply_style + analysis.check_deliverable  (loudness/TP conformance)
//   deliver_to_iamf       -> spatial.export_loom_manifest      (bed/scene/VO stems + a
//                                                                ready-to-compile iamf-loom manifest,
//                                                                validated downstream by iamf-sentinel)
//   author_dolby_adm      -> spatial.export_adm                (an ADM BWF whose OBJECTS survive
//                                                                iamf-tools' importer — the worked
//                                                                example for dolbyMetadataChunk)
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

std::vector<PromptMessage> renderDeliverToIamf(const Json& args) {
    const std::string outDir = argStr(args, "outDir", "<choose an output directory>");
    const std::string bed = argStr(args, "bed", "7.1.4");
    const std::string order = argStr(args, "sceneOrder", "0");
    const std::string target = argStr(args, "target", "iamf");
    const bool wantBed = bed != "none" && !bed.empty();
    const bool wantScene = order != "0" && !order.empty();

    std::string t;
    t += "Deliver this REAPER session as an IAMF (open, royalty-free immersive) package";
    if (wantBed) t += ", carrying the " + bed + " bed";
    if (wantScene) t += std::string(wantBed ? " plus" : ", carrying") + " the order-" + order +
                        " ambisonic scene";
    t += ", via the iamf-loom / iamf-sentinel toolchain.\n\n";
    t += "1. Inventory what will ship: confirm the bed bus and its channel count, any ambisonic "
         "scene track (ACN/SN3D and its order), and any per-language VO tracks. "
         "analysis.send_layout_inspect and spatial.get_scene_info help; report the roster before "
         "exporting.\n";
    t += "2. Check the per-mix channel budget BEFORE rendering: bed channels (stereo 2 / 5.1 6 / "
         "7.1.4 12) + (sceneOrder+1)^2 + 2 per VO language. IAMF's largest per-mix profile cap "
         "(base_enhanced) is 28 — a 7.1.4 bed + an order-3 scene + a stereo VO is 30 and loom "
         "compile will refuse it (M-416). If over budget, propose delivering the scene at order 2 "
         "(12+9+2 = 23 fits) or splitting bed and scene into separate mixes — ask me which.\n";
    t += "3. Call spatial.export_loom_manifest with \"dryRun\": true first — e.g. { "
         + std::string(wantBed ? "\"bedTrack\": <bed bus>, \"bedLayout\": \"" + bed + "\", " : "")
         + (wantScene ? "\"sceneTrack\": <scene track>, \"sceneOrder\": " + order + ", " : "")
         + "\"targets\": [{ \"format\": \"" + target + "\" }], \"outDir\": \"" + outDir +
         "\", \"dryRun\": true } plus \"voTracks\": [{ \"track\": …, \"lang\": … }] rows for any "
         "VO (preset \"youtube\" needs a \"video\" path on an mp4 target; preset \"archive\" is "
         "the flac mezzanine) — and show me the composed manifest.yaml. The dry run renders "
         "nothing; it is the pre-flight for step 2's budget and for naming problems the tool "
         "warns about.\n";
    t += "4. After I approve, re-issue without dryRun. The tool renders bit-exact 48 kHz "
         "integer-PCM WAVs in Loom's channel order (the mapping is identity — no permutation) and "
         "writes the loom: 0 manifest beside them. Report the outputs and echo the tool's `next` "
         "field verbatim — it is the exact `loom compile` command to run outside REAPER.\n";
    t += "5. Packaging and validation happen in the IAMF stack, not in REAPER: `loom compile` "
         "validates the plan (no encoder toolchain needed), `loom run` encodes/muxes/measures and "
         "gates every output through the iamf-sentinel validator. If the iamf-sentinel-mcp server "
         "is connected to this client, drive that half agent-side (loom_compile, then iamf_validate "
         "on the produced file) and relay the S-/M-code findings; otherwise hand me the commands.\n";
    t += "6. Boundaries to state honestly: object tracks are NOT carried — the export fails closed "
         "with the ADM pointer (M-309); objects ship via spatial.export_adm instead. And never "
         "invent loudness numbers — Loom measures loudness itself during packaging.\n";
    return {PromptMessage{"user", t}};
}

// author_dolby_adm — the worked example for spatial.export_adm's dolbyMetadataChunk switch.
// Its whole reason to exist: the switch is opt-in, and an opt-in nobody discovers does not exist.
// The template makes the agent state the trade BEFORE taking it, because taking it is a provenance
// assertion, not a formatting choice.
std::vector<PromptMessage> renderAuthorDolbyAdm(const Json& args) {
    const std::string bed = argStr(args, "bed", "7.1.2");
    const std::string objects = argStr(args, "objects", "");
    const std::string outPath = argStr(args, "outPath", "");
    std::string t;
    t = "Author an ADM (ITU-R BS.2076) Broadcast-Wave from this session whose OBJECTS survive "
        "ingest by iamf-tools' ADM importer, using spatial.export_adm. Bed layout: " + bed + ". ";
    t += objects.empty() ? "Use the session's object tracks (ask me which if it is ambiguous). "
                         : ("Object tracks: " + objects + ". ");
    if (!outPath.empty()) t += "Write it to " + outPath + ". ";
    t += "\n\n";
    t += "1. Start with dryRun:true and bedLayout \"" + bed + "\". Read back the planned channel "
         "count, the object roster and every entry in `warnings` — do not skip them, one of them is "
         "the point of this workflow.\n";
    t += "2. You will see an advisory saying the export carries dynamic objects but no `dbmd` "
         "chunk, so that importer takes its DEFAULT path, rejects every audioObject as \"Not under "
         "common definition\" and fails the encode outright — no IAMF file at all, not a bed-only "
         "one. That is a fact about ONE consumer (iamf-tools, as measured on the date the advisory "
         "names), not a defect in the file: a certified Dolby or EBU renderer ingests the default "
         "export fine.\n";
    t += "3. Set dolbyMetadataChunk:\"placeholder\" ONLY after telling me, in one sentence, what it "
         "costs: it emits a placeholder `dbmd` chunk AND renames the bed channels to Dolby's "
         "RoomCentric* vocabulary, which together ASSERT the file is a Dolby ADM deliverable. That "
         "assertion is the caller's to make, which is exactly why it is not the default.\n";
    t += "4. The switch FAILS CLOSED and will refuse rather than write a file the importer rejects "
         "outright: it needs 24-bit PCM, a 48 kHz or 96 kHz render, and a bed of 5.1, 7.1 or 7.1.2. "
         "7.1.4, 9.1.6 and 22.2 have NO accepted Dolby pack layout — if I asked for one of those, "
         "say so plainly and offer to route the height channels as objects instead of silently "
         "downgrading my bed.\n";
    t += "5. Re-issue without dryRun, then verify the result with analysis.adm_inspect: the chunk "
         "list must now read fmt /dbmd/chna/axml/data, and the advisory must be GONE from "
         "`warnings`. Report both — the absence of the warning is the evidence, not my say-so.\n";
    t += "6. Ingest-verify against a certified renderer before you call the deliverable good. This "
         "workflow makes a file one open-source importer accepts; that is not the same claim as "
         "Dolby Atmos conformance. For the latter run analysis.adm_profile_check, and note it does "
         "NOT require a `dbmd` — the two checks are independent and neither implies the other.\n";
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

    reg.add(Prompt{
        "deliver_to_iamf",
        "Deliver the session to IAMF (open immersive)",
        "Render bed / ambisonic-scene / VO stems and emit a ready-to-compile iamf-loom manifest via "
        "spatial.export_loom_manifest, then validate the packaged output with iamf-sentinel — the "
        "open, royalty-free IAMF delivery pipeline (see MANUAL §9, Delivering to IAMF).",
        {PromptArg{"outDir", "Directory to write the rendered stems + manifest.yaml into", true},
         PromptArg{"bed", "Bed layout: stereo, 5.1, 7.1.4, or none (default 7.1.4)", false},
         PromptArg{"sceneOrder", "Ambisonic scene order 1-4, or 0 for no scene (default 0)", false},
         PromptArg{"target", "Delivery target: iamf, mp4, or preview; presets youtube / archive (default iamf)", false}},
        renderDeliverToIamf});

    reg.add(Prompt{
        "author_dolby_adm",
        "Author an ADM BWF whose objects survive IAMF ingest",
        "Author an ITU-R BS.2076 ADM Broadcast-Wave via spatial.export_adm and, deliberately, take "
        "its dolbyMetadataChunk opt-in — a placeholder `dbmd` chunk plus RoomCentric* bed names — so "
        "iamf-tools' ADM importer keeps the objects instead of dropping them. States what the "
        "opt-in asserts before taking it, and verifies the result with analysis.adm_inspect.",
        {PromptArg{"bed", "Bed layout: 5.1, 7.1 or 7.1.2 (default 7.1.2 — larger beds have no "
                          "accepted Dolby pack layout)", false},
         PromptArg{"objects", "Object track indices or names (default: ask)", false},
         PromptArg{"outPath", "Where to write the .wav (default: the project directory)", false}},
        renderAuthorDolbyAdm});
}

}  // namespace reaper_mcp
