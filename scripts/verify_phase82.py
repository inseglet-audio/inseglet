#!/usr/bin/env python3
"""Live-verify Phase 8 Batch P8-2 — actions / command-execution passthrough (Session P8-A/B gate).

Drives action.run / action.run_by_name / action.get_toggle_state against a running REAPER + the loaded
extension, proving the D2 safety model end-to-end: a benign toggle round-trips (and is restored), the
built-in deny-list refuses a known modal/fatal action, dryRun resolves + policy-checks without running
anything, and named-command resolution works.

SAFETY: this gate NEVER fires a modal or process-fatal action. The deny check first dryRun's the target
(same policy path as a real run) and only issues a real run once dryRun has confirmed it is blocked — so
even a real action.run here cannot execute a dialog/quit. Still, run it on a SCRATCH project.

REAPER must be in the FOREGROUND (App Nap stalls the Run() pump). Stdlib only.

Coverage:
  0. surface == 97 (94 baseline + 3 P8-2).
  1. dryRun resolves an action's real REAPER name (proves kbd_getTextFromCmd + the name layer's source).
  2. benign toggle round-trip: get_toggle_state -> action.run -> state flips -> action.run -> restored.
  3. deny-list: dryRun(Close project) -> wouldRun=false; a REAL action.run then returns action_denied
     and the project is untouched (proven by tools still answering afterwards).
  4. run_by_name: a bogus name -> action_not_found; a real named command (dryRun, never executed)
     either resolves or reports not-found (SWS/extension-dependent) — both are valid.
  5. get_toggle_state read-only shape.

Usage:
    python3 verify_phase82.py                  # auto-find reaper_mcp.json
    python3 verify_phase82.py /path/to/reaper_mcp.json
    python3 verify_phase82.py --cleanup        # (no persistent state created; accepted for symmetry)
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

# Well-known benign toggle: "Options: Toggle metronome". The round-trip self-validates (skips if this
# id is not a toggle on this build), so an id drift degrades to a SKIP, never a false FAIL.
METRONOME = 40364
# A known modal/fatal action for the deny check: "File: Close current project" (numeric deny layer).
CLOSE_PROJECT = 40860

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}

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
    """Return structuredContent. NOTE: a tool-level makeError (e.g. action_denied) is NOT a transport
    isError — it comes back in structuredContent with an 'error' field, which callers inspect."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_phase82", "version": "0"}})

passed = failed = skipped = 0
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

def skip(label, detail=""):
    global skipped
    skipped += 1
    print(f"  [SKIP] {label}" + (f"  — {detail}" if detail else ""))

# ------------------------------------------------------------------------------------------------
print("== 0. surface == 97 ==")
initialize()
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == 97, "tool surface == 97 (94 + 3 P8-2)", f"count={enum.get('count')}")
for t in ("action.run", "action.run_by_name", "action.get_toggle_state"):
    check(any(x.get("name") == t for x in rpc("tools/list").get("tools", [])), f"{t} present in tools/list")

print("\n== 1. dryRun resolves the real REAPER action name (name-layer source) ==")
dr = call("action.run", command=METRONOME, dryRun=True)
name = dr.get("name", "")
check(dr.get("dryRun") is True and dr.get("wouldRun") is True, "dryRun(metronome) -> wouldRun=true")
check(isinstance(name, str) and len(name) > 0, "dryRun resolved a non-empty action name", repr(name))
if "metronome" not in name.lower():
    skip("id 40364 name-sanity", f"resolved name {name!r} — proceeding with self-validating toggle")

print("\n== 2. benign toggle round-trip (get_toggle_state <-> action.run), restored ==")
st0 = call("action.get_toggle_state", command=METRONOME)
avail = st0.get("available") is True
if not avail:
    skip("metronome toggle round-trip", f"id {METRONOME} is not a toggle here (toggleState={st0.get('toggleState')})")
else:
    s0 = st0.get("toggleState")
    r1 = call("action.run", command=METRONOME)
    check(r1.get("ok") is True and r1.get("command") == METRONOME, "action.run(metronome) ok")
    s1 = call("action.get_toggle_state", command=METRONOME).get("toggleState")
    check(s1 != s0, "toggle state flipped after action.run", f"{s0} -> {s1}")
    check(r1.get("toggleState") == s1, "action.run echoed the post-run toggle state", f"{r1.get('toggleState')}")
    call("action.run", command=METRONOME)  # flip back
    s2 = call("action.get_toggle_state", command=METRONOME).get("toggleState")
    check(s2 == s0, "second action.run restored the original toggle state", f"{s2} == {s0}")

print("\n== 3. deny-list refuses a modal/fatal action (dryRun-guarded, project untouched) ==")
proj_before = call("transport.get_state")  # a read that must still work after the (blocked) call
dcheck = call("action.run", command=CLOSE_PROJECT, dryRun=True)
blocked = dcheck.get("wouldRun") is False
check(blocked, "dryRun(Close project) -> wouldRun=false (deny-list)", dcheck.get("reason", ""))
if blocked:
    # dryRun (same evaluate() path) says blocked => a real run cannot execute it. Prove enforcement.
    real = call("action.run", command=CLOSE_PROJECT)
    check(real.get("error") == "action_denied", "real action.run(Close project) -> action_denied",
          real.get("detail", ""))
    # the project is still there and the server still answers (nothing was closed/hung)
    proj_after = call("transport.get_state")
    check(isinstance(proj_after, dict) and "playState" in proj_after,
          "server still responsive + project intact after the blocked call")
else:
    skip("real deny enforcement", "dryRun did not report the action blocked — NOT issuing a real run")

print("\n== 4. run_by_name resolution ==")
nf = call("action.run_by_name", name="_NOT_A_REAL_COMMAND_ID_", dryRun=True)
check(nf.get("error") == "action_not_found", "run_by_name(bogus) -> action_not_found")
# A real SWS named command, dryRun only (never executed — _SWS_ABOUT would open a modal if run):
about = call("action.run_by_name", name="_SWS_ABOUT", dryRun=True)
if about.get("error") == "action_not_found":
    skip("run_by_name(_SWS_ABOUT)", "SWS not installed — named lookup returned not-found (valid)")
else:
    check(about.get("dryRun") is True and int(about.get("command", 0)) != 0,
          "run_by_name resolved a named command via NamedCommandLookup", f"cmd={about.get('command')}")

print("\n== 5. get_toggle_state read-only shape ==")
gs = call("action.get_toggle_state", command=METRONOME)
check("toggleState" in gs and "available" in gs and gs.get("command") == METRONOME,
      "get_toggle_state returns command/toggleState/available")

# ------------------------------------------------------------------------------------------------
print(f"\n{'='*60}\n  PASS={passed}  FAIL={failed}  SKIP={skipped}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: GREEN — Phase 8 Batch P8-2 (actions passthrough) verified live.")
