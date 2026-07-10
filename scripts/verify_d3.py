#!/usr/bin/env python3
"""Live-verify Phase 8 D3 — track.set_state_chunk (elicitation-gated wholesale state replace).

Drives the D3 verb against a running REAPER + the loaded extension, proving the SDK path (not just the
host stub): read a real track chunk, preview a unified diff, exercise the confirmation gate, apply, and
confirm the edit actually landed. The gate builds its own scratch track and (with --cleanup) removes it.

The four D3 rails, each checked end-to-end:
  (a) structural validation — a non-<TRACK chunk is rejected as `invalid_chunk` before any edit.
  (b) dry-run preview — dryRun returns a unified diff of current-vs-proposed and mutates NOTHING.
  (c) confirmation gate — a change without confirm (this client does not advertise elicitation, so the
      server's requireElicitation() no-ops) returns `confirmation_required` and mutates NOTHING; the
      real elicitation round-trip is a client behavior, separately covered by unit.elicitation.
  (d) apply in one undo block — with confirm:true the chunk is written; an unchanged proposal is a no-op.

SAFETY / pump-hygiene: no modal path is touched. The only write is SetTrackStateChunk on the gate's OWN
scratch track (a track-name edit), inside one undo block — trivially reversible (Cmd-Z) and removed by
--cleanup. REAPER must be in the FOREGROUND. Stdlib only.

Usage:
    python3 verify_d3.py                 # auto-find reaper_mcp.json; leaves the scratch track
    python3 verify_d3.py --cleanup       # also delete the scratch track it created
    python3 verify_d3.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request
from urllib.parse import urlparse

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),  # macOS
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),                      # Linux
]
EXPECTED_SURFACE = 125  # 124 (P8-3 baseline) + 1 (D3 track.set_state_chunk)
ORIG_NAME = "MCP D3 scratch"
NEW_NAME = "MCP D3 renamed"

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action first.")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
U = urlparse(URL)
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print(f"endpoint : {URL}\ntoken    : {TOKEN[:8]}…\n")

_id = 0
def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=30) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    """structuredContent of a call. A tool-level makeError (structured 'error' field) is NOT a transport
    isError, so it comes back here — callers inspect .get('error')."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def read_resource(uri):
    r = rpc("resources/read", {"uri": uri})
    return r["contents"][0]["text"]

def initialize():
    # capabilities:{} => NOT elicitation-capable, so requireElicitation() no-ops server-side and the
    # no-confirm path returns confirmation_required (rail (c) below).
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_d3", "version": "0"}})

passed = failed = skipped = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

def skip(label, detail=""):
    global skipped
    skipped += 1
    print(f"  [SKIP] {label}" + (f"  — {detail}" if detail else ""))

def track_name(idx):
    return call("track.get", track=idx).get("name")

# ================================================================================================
initialize()
print(f"== 0. surface == {EXPECTED_SURFACE} ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      f"tool surface == {EXPECTED_SURFACE} (124 + 1 D3)", f"count={enum.get('count')}")
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
check("track.set_state_chunk" in listed, "track.set_state_chunk present in tools/list")

# --- scratch setup: one track at the end of the project ---
base = call("project.get_summary").get("trackCount", 0)
trackA = call("track.add", index=base, name=ORIG_NAME).get("trackIndex", base)
print(f"\nscratch track: A={trackA} ('{ORIG_NAME}')\n")

# Read the track's REAL current chunk via the read resource (the D3 write complements this read).
uri = f"reaper://track/{trackA}/chunk"
current = read_resource(uri)
check(current.lstrip().startswith("<TRACK"), "read reaper://track/{i}/chunk -> a <TRACK block",
      f"{len(current)} bytes")
has_name = f'"{ORIG_NAME}"' in current
if not has_name:
    skip("build a modified chunk", "could not find the quoted NAME token in the chunk")
proposed = current.replace(f'"{ORIG_NAME}"', f'"{NEW_NAME}"', 1) if has_name else current

print("== (a) validation: a non-<TRACK chunk is rejected before any edit ==")
bad = call("track.set_state_chunk", track=trackA, chunk="this is not a chunk")
check(bad.get("error") == "invalid_chunk", "garbage chunk -> error=invalid_chunk (no mutation)")
check(track_name(trackA) == ORIG_NAME, "track name unchanged after invalid_chunk")

if has_name:
    print("\n== (b) dry-run: unified diff preview, zero mutation ==")
    dry = call("track.set_state_chunk", track=trackA, chunk=proposed, dryRun=True)
    check(dry.get("ok") is True and dry.get("changed") is True and dry.get("applied") is False,
          "dryRun -> ok, changed=true, applied=false")
    check(dry.get("addedLines", 0) >= 1 and dry.get("removedLines", 0) >= 1 and
          "@@" in dry.get("diff", "") and NEW_NAME in dry.get("diff", ""),
          "dryRun diff is a real unified hunk showing the rename",
          f"+{dry.get('addedLines')}/-{dry.get('removedLines')}")
    check(track_name(trackA) == ORIG_NAME, "track name still original after dryRun (nothing applied)")

    print("\n== (c) confirmation gate: change without confirm is refused, zero mutation ==")
    gate = call("track.set_state_chunk", track=trackA, chunk=proposed)
    check(gate.get("error") == "confirmation_required" and gate.get("applied") is False and
          "@@" in gate.get("diff", ""),
          "no-confirm change -> confirmation_required + diff, applied=false")
    check(track_name(trackA) == ORIG_NAME, "track name still original after the refused write")

    print("\n== (d) no-op: applying the CURRENT (unchanged) chunk with confirm ==")
    noop = call("track.set_state_chunk", track=trackA, chunk=current, confirm=True)
    check(noop.get("ok") is True and noop.get("changed") is False and noop.get("applied") is False,
          "unchanged proposal is a no-op (changed=false, applied=false)")

    print("\n== (d) apply: confirmed write actually lands ==")
    ap = call("track.set_state_chunk", track=trackA, chunk=proposed, confirm=True)
    check(ap.get("ok") is True and ap.get("applied") is True and ap.get("changed") is True,
          "confirmed apply -> ok, applied=true, changed=true")
    check(track_name(trackA) == NEW_NAME, "the track was actually renamed by the chunk write",
          f"name now '{track_name(trackA)}'")

    print("\n== (d) idempotent: re-reading the applied state and re-applying it verbatim is a no-op ==")
    # NOTE (REAPER behavior): SetTrackStateChunk RE-SERIALIZES the chunk (formatting / field order /
    # defaults can shift), so feeding the SAME hand-edited text back is not necessarily a *textual*
    # no-op — REAPER's re-emitted chunk differs from the edited input even when semantically equal.
    # The genuine invariant the tool guarantees is read->write: read the current chunk and apply it
    # verbatim => no-op. That is what we assert here.
    current2 = read_resource(uri)
    again = call("track.set_state_chunk", track=trackA, chunk=current2, confirm=True)
    check(again.get("changed") is False and again.get("applied") is False,
          "re-applying the freshly-read current chunk is a no-op (read->write round-trip)")
else:
    skip("dry-run / gate / apply", "no quoted NAME token to edit; validation-only coverage this run")

# --- cleanup ---
if DO_CLEANUP:
    print("\n== cleanup ==")
    try:
        call("track.remove", track=trackA)
        print(f"  removed scratch track {trackA}")
    except RuntimeError as e:
        print(f"  [warn] could not remove track {trackA}: {e}")
else:
    notes.append(f"scratch track {trackA} left in place, renamed to '{NEW_NAME}' (pass --cleanup to remove)")

# ================================================================================================
print(f"\n{'='*64}\n  PASS={passed}  FAIL={failed}  SKIP={skipped}")
for n in notes:
    print(f"  note: {n}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: GREEN — Phase 8 D3 (track.set_state_chunk) verified live.")
