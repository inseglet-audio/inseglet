# tests/

Two tiers:

- **`unit/`** — host-build C++ tests with no REAPER dependency (queue budget/marshaling, schema
  validation, dispatch). Run via `ctest` after `cmake --build`.
- **`integration/`** — opt-in tests that need a REAPER install. Set `REAPER_BIN` and run
  `ctest -R integration`. `run_conformance.py` launches REAPER with a golden project, drives the
  MCP server as a client, and asserts on protocol conformance + resulting project state + rendered
  deliverable measurements. `fixtures/` (add) holds golden `.RPP` projects and expected outputs.

Planned coverage as phases land:
- Phase 1: protocol round-trips, structured-output schema conformance.
- Phase 2: golden-project before/after diffs for every mutating tool; ReaScript-fallback oracle cross-check.
- Phase 4: spatial correctness — ambisonic encode/decode round-trip, ACN/SN3D vs FuMa channel order,
  per-bed LUFS/true-peak on rendered immersive masters.
- Soak/latency: sustained tool throughput without UI stalls; per-`Run()` budget respected.
