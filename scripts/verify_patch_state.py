#!/usr/bin/env python3
"""Live-verify track.patch_state — targeted top-level chunk-key patch (post-Phase-8).

Drives track.patch_state against a running REAPER + the loaded extension, proving the SDK write path
(GetTrackStateChunk -> targeted top-level key replace -> SetTrackStateChunk) end-to-end, plus the
D3-shared rails: structural validation, a dry-run unified diff that mutates nothing, the
confirm/elicitation gate, a single undo block, and the no-op / read->write idempotency invariant.

The gate builds ONE scratch track, patches its top-level keys (NAME, PEAKCOL), verifies each change by
reading the chunk resource back, and (with --cleanup) removes the track. Nothing here opens a dialog.
REAPER must be in the FOREGROUND. Stdlib only.

Usage:
    python3 verify_patch_state.py                 # auto-find reaper_mcp.json; leaves the scratch track
    python3 verify_patch_state.py --cleanup       # also delete the scratch track it created
    python3 verify_patch_state.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 139  # 138 (P8-4) + 1 (track.patch_state)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action first.")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print("endpoint : %s\ntoken    : %s...\n" % (URL, TOKEN[:8]))

_id = 0
def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method,
                       "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=60) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError("%s -> %s" % (method, j["error"]))
    return j["result"]

def call(tool_name, **arguments):
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError("%s isError: %s" % (tool_name, r.get("content")))
    return r.get("structuredContent", {})

def raw(tool_name, **arguments):
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})

def read_chunk(idx):
    res = rpc("resources/read", {"uri": "reaper://track/%d/chunk" % idx})
    return res["contents"][0]["text"]

def top_value(chunk, key):
    """The value (remainder of the line) of the first TOP-LEVEL key of the <TRACK> block."""
    depth = 0
    for line in chunk.split("\n"):
        s = line.strip()
        if not s:
            continue
        if s == ">":
            depth -= 1
            continue
        if s.startswith("<"):
            depth += 1
            continue
        if depth == 1:
            parts = s.split(None, 1)
            if parts[0] == key:
                return parts[1] if len(parts) > 1 else ""
    return None

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_patch_state", "version": "0"}})

passed = failed = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (138 P8-4 + 1 patch_state)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
check("track.patch_state" in listed, "track.patch_state present in tools/list")

# --- scratch setup: one track at the end of the project ---
base = call("project.get_summary").get("trackCount", 0)
track = call("track.add", index=base, name="MCP patch scratch").get("trackIndex", base)
print("\nscratch track: %d\n" % track)
orig_name = call("track.get", track=track).get("name")
check(orig_name == "MCP patch scratch", "scratch track created with the expected NAME",
      "name=%r" % orig_name)

print("== 1. dryRun previews a NAME patch and mutates NOTHING ==")
dry = call("track.patch_state", track=track,
           patches=[{"key": "NAME", "value": '"Renamed Via Patch"'}], dryRun=True)
check(dry.get("ok") is True and dry.get("changed") is True and dry.get("applied") is False
      and dry.get("keys") == ["NAME"] and bool(dry.get("diff")),
      "dryRun -> ok+changed, applied:false, keys=[NAME], non-empty diff",
      "keys=%s applied=%s" % (dry.get("keys"), dry.get("applied")))
check(call("track.get", track=track).get("name") == orig_name,
      "dryRun did NOT change the track NAME")

print("\n== 2. confirm gate: no confirm + no elicitation -> confirmation_required, no mutation ==")
gate = raw("track.patch_state", track=track, patches=[{"key": "NAME", "value": '"Should Not Apply"'}])
gsc = gate.get("structuredContent", {})
check(gsc.get("error") == "confirmation_required" and gsc.get("applied") is False,
      "unconfirmed patch -> error=confirmation_required, applied:false", "error=%s" % gsc.get("error"))
check(call("track.get", track=track).get("name") == orig_name,
      "confirm-gate refusal left the NAME unchanged")

print("\n== 3. real confirmed apply: patch NAME -> read-back proves the SDK write path ==")
new_name = "Patched Kick 909"
ap = call("track.patch_state", track=track,
          patches=[{"key": "NAME", "value": '"%s"' % new_name}], confirm=True)
check(ap.get("ok") is True and ap.get("applied") is True and ap.get("changed") is True
      and ap.get("keys") == ["NAME"],
      "confirmed patch -> ok+applied+changed, keys=[NAME]", "applied=%s" % ap.get("applied"))
check(call("track.get", track=track).get("name") == new_name,
      "track.get reflects the patched NAME", "name=%r" % call("track.get", track=track).get("name"))

print("\n== 4. multi-key patch (NAME + PEAKCOL) in ONE call, verified via the chunk read ==")
before = read_chunk(track)
orig_peak = top_value(before, "PEAKCOL")
mk = call("track.patch_state", track=track, patches=[
    {"key": "NAME", "value": '"Multi Patched"'},
    {"key": "PEAKCOL", "value": "33554431"}], confirm=True)  # 0x1FFFFFF: custom-color enable bit set
check(mk.get("applied") is True and set(mk.get("keys", [])) == {"NAME", "PEAKCOL"},
      "multi-key patch applied both NAME + PEAKCOL in one call", "keys=%s" % mk.get("keys"))
after = read_chunk(track)
check(top_value(after, "NAME") == '"Multi Patched"',
      "chunk read-back: NAME carries the patched value", "NAME=%r" % top_value(after, "NAME"))
check(top_value(after, "PEAKCOL") is not None and top_value(after, "PEAKCOL") != orig_peak,
      "chunk read-back: PEAKCOL changed from its original value",
      "%r -> %r" % (orig_peak, top_value(after, "PEAKCOL")))

print("\n== 5. no-op / read->write idempotency: patch a key to its CURRENT value ==")
cur_name = top_value(after, "NAME")
noop = call("track.patch_state", track=track,
            patches=[{"key": "NAME", "value": cur_name}], confirm=True)
check(noop.get("changed") is False and noop.get("applied") is False,
      "patching NAME to its current value -> changed:false, applied:false (no-op)",
      "changed=%s" % noop.get("changed"))

print("\n== 6. key_not_found: an absent top-level key fails closed, mutating nothing ==")
kn = raw("track.patch_state", track=track, patches=[{"key": "NOTAREALKEY", "value": "1"}], confirm=True)
check(kn.get("structuredContent", {}).get("error") == "key_not_found",
      "patching an absent top-level key -> error=key_not_found",
      "error=%s" % kn.get("structuredContent", {}).get("error"))
check(top_value(read_chunk(track), "NAME") == cur_name,
      "key_not_found mutated nothing (NAME still the multi-patched value)")

print("\n== 7. invalid_patch: a duplicate key is rejected before any REAPER call ==")
dupe = raw("track.patch_state", track=track, patches=[
    {"key": "NAME", "value": '"A"'}, {"key": "NAME", "value": '"B"'}], confirm=True)
check(dupe.get("structuredContent", {}).get("error") == "invalid_patch",
      "duplicate key -> error=invalid_patch")

# --- cleanup ---
if DO_CLEANUP:
    print("\n== cleanup ==")
    call("track.remove", track=track)
    check(call("project.get_summary").get("trackCount") == base, "scratch track removed")
else:
    notes.append("scratch track %d left in place (pass --cleanup to remove)" % track)

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
