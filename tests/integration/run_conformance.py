#!/usr/bin/env python3
"""run_conformance.py — integration/protocol-conformance harness (Phase 1+).

This is the skeleton for the integration tier:

  1. Launch REAPER (REAPER_BIN) with a scripted golden project from tests/fixtures/.
  2. Wait for the reaper_mcp extension's MCP server to come up (poll the port it prints, or read a
     handshake file), OR start the bootstrap Lua fallback for the oracle comparison.
  3. Drive it as an MCP client: initialize -> tools/list -> tools/call, and assert on:
       - protocol conformance (JSON-RPC framing, structured output matches each tool's outputSchema)
       - resulting REAPER project state (re-open the .RPP, or query via a read-only tool)
       - rendered-file measurements (LUFS / true-peak / channel count / format) for render tools
  4. Cross-check overlapping tools against the ReaScript fallback oracle (bootstrap/).

Kept as TODO stubs so the repo has a runnable, extendable entry point from day one.
"""
import os
import sys


def main() -> int:
    reaper_bin = os.environ.get("REAPER_BIN")
    if not reaper_bin:
        print("SKIP: set REAPER_BIN to run integration tests")
        return 0  # skip, don't fail

    # TODO(phase1): launch REAPER headless-ish with a fixture project and connect an MCP client.
    # TODO(phase1): assert initialize/tools.list/tools.call round-trips and structured-output schemas.
    # TODO(phase2): golden-project before/after diffs per mutating tool.
    # TODO(phase4): spatial correctness — ambisonic encode/decode round-trip, channel-order (ACN/SN3D),
    #               per-bed LUFS/true-peak on rendered immersive deliverables.
    print("reaper_mcp conformance harness: skeleton only (see TODOs).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
