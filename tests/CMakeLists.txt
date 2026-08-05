# tests/CMakeLists.txt
#
# Two tiers of tests:
#   1. Unit — pure C++ pieces that don't need REAPER (queue budget, transport/protocol conformance).
#   2. Integration — headless-REAPER + golden-project + protocol conformance (driven from Python).

# --- Unit: main-thread marshaling queue (header-only, no deps) ---
add_executable(reaper_mcp_unit unit/test_main_thread_queue.cpp)
target_include_directories(reaper_mcp_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.main_thread_queue COMMAND reaper_mcp_unit)

# --- Unit: resource-subscription hub (header-only, no deps) ---
add_executable(reaper_mcp_subs_test unit/test_subscription_hub.cpp)
target_include_directories(reaper_mcp_subs_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(reaper_mcp_subs_test PRIVATE Threads::Threads)
add_test(NAME unit.subscription_hub COMMAND reaper_mcp_subs_test)

# --- Unit: batch-e head-tracking OSC parser + orientation math (header-only, no deps, no sockets) ---
add_executable(reaper_mcp_headtrack_test unit/test_head_track.cpp)
target_include_directories(reaper_mcp_headtrack_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(reaper_mcp_headtrack_test PRIVATE Threads::Threads)
add_test(NAME unit.head_track COMMAND reaper_mcp_headtrack_test)

# --- Unit: MCP protocol/transport over real HTTP (no REAPER) ---
# Links the SDK-free host core (transport + registry + tool bodies' fallback path).
add_executable(reaper_mcp_protocol_test unit/test_mcp_server.cpp)
target_link_libraries(reaper_mcp_protocol_test PRIVATE reaper_mcp_hostcore)
add_test(NAME unit.mcp_protocol COMMAND reaper_mcp_protocol_test)

# --- Unit: Phase-5a MCP prompt registry + expert-workflow templates (no REAPER) ---
# Links the host core for registerExpertPrompts() and the json include path.
add_executable(reaper_mcp_prompts_test unit/test_prompts.cpp)
target_link_libraries(reaper_mcp_prompts_test PRIVATE reaper_mcp_hostcore)
add_test(NAME unit.prompts COMMAND reaper_mcp_prompts_test)

# --- Unit: Phase-5a agent-ergonomics substrate (composite_support.h; header-only, no REAPER) ---
add_executable(reaper_mcp_ergonomics_test unit/test_ergonomics.cpp)
target_include_directories(reaper_mcp_ergonomics_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_ergonomics_test PRIVATE Threads::Threads)
add_test(NAME unit.ergonomics COMMAND reaper_mcp_ergonomics_test)

# --- Unit: Phase-5b spatial composite planning core (placement_defaults.h + spatial_verbs.h) ---
# Pure SDK-free logic: descriptor->geometry table, target parsing, placement resolution, dryRun plans.
add_executable(reaper_mcp_spatial_verbs_test unit/test_spatial_verbs.cpp)
target_include_directories(reaper_mcp_spatial_verbs_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_spatial_verbs_test PRIVATE Threads::Threads)
add_test(NAME unit.spatial_verbs COMMAND reaper_mcp_spatial_verbs_test)

# --- Unit: Phase-5c deliverable-spec table + pass/fail evaluator (deliverable_specs.h) ---
# Pure SDK-free logic: the named specs, name/alias resolution, per-metric conformance.
add_executable(reaper_mcp_deliverable_specs_test unit/test_deliverable_specs.cpp)
target_include_directories(reaper_mcp_deliverable_specs_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_deliverable_specs_test PRIVATE Threads::Threads)
add_test(NAME unit.deliverable_specs COMMAND reaper_mcp_deliverable_specs_test)

# --- Unit: Phase-5d style palette + resolver (style_palette.h) ---
# Pure SDK-free logic: named styles, in-box/detected resolution, limiter-ceiling fill, immersive flags.
add_executable(reaper_mcp_style_palette_test unit/test_style_palette.cpp)
target_include_directories(reaper_mcp_style_palette_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_style_palette_test PRIVATE Threads::Threads)
add_test(NAME unit.style_palette COMMAND reaper_mcp_style_palette_test)

# --- Unit: Phase-5e deterministic macro-DSL core (dsl/dsl.h) ---
# Pure SDK-free logic: tokenizer, typed value coercion, verb alias table + key sugar + dotted nesting,
# structured parse errors, and the explain-the-diff rendering.
add_executable(reaper_mcp_dsl_test unit/test_dsl.cpp)
target_include_directories(reaper_mcp_dsl_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_dsl_test PRIVATE Threads::Threads)
add_test(NAME unit.dsl COMMAND reaper_mcp_dsl_test)

# --- Unit: Phase-6(d) async-elicitation substrate (elicitation.h + requireElicitation) ---
# Pure SDK-free logic: the pending-elicitation correlator (create/deliver/cancel/timeout/poll,
# idempotency) and the requireElicitation() control flow via the thread-local ElicitationScope.
add_executable(reaper_mcp_elicitation_test unit/test_elicitation.cpp)
target_include_directories(reaper_mcp_elicitation_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_elicitation_test PRIVATE Threads::Threads)
add_test(NAME unit.elicitation COMMAND reaper_mcp_elicitation_test)

# --- Unit: Phase-8 (P8-2) command-execution safety policy (actions_policy.h; header-only, no REAPER) ---
# Pure SDK-free logic: the numeric deny-set, the display-name deny layer, and the optional allow-list
# (default deny-net vs locked-down allow-list-only modes).
add_executable(reaper_mcp_actions_test unit/test_actions.cpp)
target_include_directories(reaper_mcp_actions_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.actions COMMAND reaper_mcp_actions_test)

# --- Unit: Batch B2 groove/quantize math (groove.h; header-only, no REAPER) ---
# Pure SDK-free logic: quantize snap + swing, velocity scaling, transpose bounds, stretch,
# groove-template extract/apply, legato, and the deterministic humanize PRNG.
add_executable(reaper_mcp_groove_test unit/test_groove.cpp)
target_include_directories(reaper_mcp_groove_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.groove COMMAND reaper_mcp_groove_test)

# --- Unit: Batch M1 immersive-metering DSP (ambisonic_meter.h; header-only, no REAPER) ---
# Pure SDK-free DSP: WAV reader, per-channel level/peak/true-peak/K-weighting, inter-channel
# correlation, and the ambisonic field (DirAC intensity DoA + diffuseness, Gerzon rV/rE vectors).
add_executable(reaper_mcp_meter_test unit/test_meter.cpp)
target_include_directories(reaper_mcp_meter_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.meter COMMAND reaper_mcp_meter_test)

# --- Unit: Batch O1 ADM (BS.2076) BWF authoring + inspection (adm_bwf.h; header-only, no REAPER) ---
# Pure SDK-free serialization: model -> chna/axml/RIFF/BW64, spherical->cartesian, BS.2051 speaker
# positions, and the round-trip parser (analysis.adm_inspect's engine).
add_executable(reaper_mcp_adm_test unit/test_adm.cpp)
target_include_directories(reaper_mcp_adm_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_adm_test PRIVATE Threads::Threads)
add_test(NAME unit.adm COMMAND reaper_mcp_adm_test)

# --- Unit: Batch O3 Dolby Atmos Master ADM Profile conformance (adm_profile.h; header-only, no REAPER) -
# Pure SDK-free normalize/validate: force cartesian, collapse extent, strip divergence/importance, the
# interpolationLength ramp + cartesian clamp, and the file-level conformance validator (adm_profile_check).
add_executable(reaper_mcp_adm_profile_test unit/test_adm_profile.cpp)
target_include_directories(reaper_mcp_adm_profile_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_adm_profile_test PRIVATE Threads::Threads)
add_test(NAME unit.adm_profile COMMAND reaper_mcp_adm_profile_test)

# --- Unit: Batch A Dolby Atmos Master File (DAMF) triad serializer (damf.h; no REAPER) ---
# Pure SDK-free logic: adm-spherical -> DAMF room [x,y,z] coordinate map, the big-endian CAF essence
# writer, the .atmos / .atmos.metadata YAML emitters, and the inspectTriad round-trip.
add_executable(reaper_mcp_damf_test unit/test_damf.cpp)
target_include_directories(reaper_mcp_damf_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_damf_test PRIVATE Threads::Threads)
add_test(NAME unit.damf COMMAND reaper_mcp_damf_test)

# --- Unit: BS.1770-4 bed channel-weight table (bed_weights.h; header-only, no REAPER) ---
# Pure SDK-free logic: the F33-conformant weight vectors (1.41 only at ear level within |az| 60-120
# inclusive; rears 1.0, heights 1.0, LFE 0; 22.2 documented-unweighted), the SMPTE label/LFE pins,
# and the quantitative power-domain delta (a wrong 1.41 cell == 10*log10(1.41) dB, via gatedLoudness).
add_executable(reaper_mcp_bed_weights_test unit/test_bed_weights.cpp)
target_include_directories(reaper_mcp_bed_weights_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.bed_weights COMMAND reaper_mcp_bed_weights_test)
# --- Unit: Batch P2 Inseglet -> iamf-loom bridge core (loom_manifest.h; header-only, no REAPER) ---
# Pure SDK-free logic: the Inseglet<->Loom channel-order mapping pin, the identity-tone verification
# of the Loom-order WAV writer (B4 method, independent in-test RIFF reader + Goertzel), the golden
# `loom: 0` manifest / season.yaml emissions, and the emission rules (never a loudness value; M-207
# slugging; the S-208 default-name trap).
add_executable(reaper_mcp_loom_manifest_test unit/test_loom_manifest.cpp)
target_include_directories(reaper_mcp_loom_manifest_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_loom_manifest_test PRIVATE Threads::Threads)
add_test(NAME unit.loom_manifest COMMAND reaper_mcp_loom_manifest_test)


# --- Unit: Batch L1 object send-layout orchestration core (send_layout.h; header-only, no REAPER) ---
# Pure SDK-free logic: the bed-first renderer input roster, the send I_SRCCHAN/I_DSTCHAN encode/decode
# round-trip, the Dolby master constraint validator, the idempotent reconciliation diff, and the
# read-back layout inspect validator.
add_executable(reaper_mcp_send_layout_test unit/test_send_layout.cpp)
target_include_directories(reaper_mcp_send_layout_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.send_layout COMMAND reaper_mcp_send_layout_test)

# --- Unit: Batch T1 per-object decode-coverage timeline core (decode_timeline.h; header-only, no REAPER) -
# Pure SDK-free geometry: az/el -> direction, SN3D encode -> projection-decode per-speaker energy, the
# dominant-speaker migration path, dwell fractions, angular travel, and the time-weighted coverage footprint.
add_executable(reaper_mcp_decode_timeline_test unit/test_decode_timeline.cpp)
target_include_directories(reaper_mcp_decode_timeline_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.decode_timeline COMMAND reaper_mcp_decode_timeline_test)

# --- Unit: B1 R1/R2 intent-sidecar emitter core (intent_sidecar.h; header-only, no REAPER) ---
# Pure SDK-free logic: the judged-layout second witness, byte-golden schema-v0 emissions, the
# decode-prediction identity with decode_timeline, the block reach-time convention, the
# forbidden-key scan (never a deliverable-loudness key), and the consumer measurement mirrors.
add_executable(reaper_mcp_intent_sidecar_test unit/test_intent_sidecar.cpp)
target_include_directories(reaper_mcp_intent_sidecar_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${REAPER_MCP_DEP_INCLUDE_DIRS})
target_link_libraries(reaper_mcp_intent_sidecar_test PRIVATE Threads::Threads)
add_test(NAME unit.intent_sidecar COMMAND reaper_mcp_intent_sidecar_test)

# --- Unit: D3 representation-aware positional-param discovery (position_params.h) ---
# Pure SDK-free logic: the az/el-first classifier (angle rosters never letter-scanned; quaternion
# components never matched), set_source_position's strict source-token semantics preserved, and
# the angle-space composition of partial cartesian requests. Carries the negative control that
# REPRODUCES the pre-D3 defect (bare-letter scan -> "Quaternion X" / "aZimuth angle"-as-z).
add_executable(reaper_mcp_position_params_test unit/test_position_params.cpp)
target_include_directories(reaper_mcp_position_params_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit.position_params COMMAND reaper_mcp_position_params_test)

# --- Integration tests (require a REAPER install; opt-in) ---
# Run: ctest -R integration  (needs REAPER_BIN env var pointing at the REAPER executable)
add_test(
  NAME integration.protocol_conformance
  COMMAND ${CMAKE_COMMAND} -E env REAPER_BIN=$ENV{REAPER_BIN}
          python3 ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_conformance.py
)
set_tests_properties(integration.protocol_conformance PROPERTIES DISABLED $<NOT:$<BOOL:$ENV{REAPER_BIN}>>)
