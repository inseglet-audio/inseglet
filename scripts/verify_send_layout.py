#!/usr/bin/env python3
"""Live-verify Batch L1 — object send-layout orchestration.

Exercises, end-to-end on the real SDK path against a running REAPER:
  * spatial.orchestrate_sends    — idempotent bed + mono-object -> external Dolby Atmos Renderer / DAPS
                                   send layout on the canonical 1-based input roster (bed on channels
                                   1..bedW, then one mono renderer channel per object). Reuse/fix/add,
                                   report/prune strays, fail closed on the Dolby master limits.
  * analysis.send_layout_inspect — read the renderer bus's incoming layout back and QC it (roster
                                   reconstruction, collisions/gaps/over-limit/unrouted).

The roster/encode/reconcile/inspect math is proven DETERMINISTICALLY off-REAPER in unit.send_layout.
This gate proves the TOOLS run end-to-end on real REAPER routing: the sends land on the right renderer
channels, re-running is idempotent (no duplicate sends), the inspector agrees, strays are pruned on
request, and the master limits fail closed. It opens its own scratch project (so the user's project is
untouched and track discovery is deterministic), builds scratch tracks, and can clean them up.

Usage:
    python3 verify_send_layout.py            # auto-find reaper_mcp.json; leaves scratch project
    python3 verify_send_layout.py --cleanup  # remove scratch tracks at the end
    python3 verify_send_layout.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 185  # 183 (v1.3.0) + 2 (Batch L1: orchestrate_sends + send_layout_inspect)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action first.")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print("endpoint : %s\ntoken    : %s...\nversion  : %s\n" % (URL, TOKEN[:8], disc.get("version")))

_id = 0
def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method,
                       "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=120) as r:
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

def err_code(result):
    sc = result.get("structuredContent", {})
    return sc.get("error") if isinstance(sc, dict) else None

def failed_closed(result):
    return result.get("isError") is True or err_code(result) is not None

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_send_layout", "version": "0"}})

passed = failed = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def add_track(name):
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

def issue_codes(insp):
    return {i.get("code") for i in insp.get("issues", [])}

def recv_by_channel(insp):
    """Map renderer 1-based channel -> (sourceTrack, width) from an inspect readout."""
    return {r.get("rendererChannel"): (r.get("sourceTrack"), r.get("width"))
            for r in insp.get("receives", [])}

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (183 + 2 Batch L1)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
l1_tools = ["spatial.orchestrate_sends", "analysis.send_layout_inspect"]
check(all(n in listed for n in l1_tools), "both Batch-L1 tools present in tools/list",
      "missing=%s" % [n for n in l1_tools if n not in listed])

# Fresh scratch project so discovery + counts are deterministic and the user's project is untouched.
RPP = "/tmp/mcp_sendlayout.rpp"
with open(RPP, "w") as fh:
    fh.write('<REAPER_PROJECT 0.1 "7.0" 0\n  SAMPLERATE 48000 1 0\n  TEMPO 120 4 4\n>\n')
op = raw("project.open", path=RPP, discardChanges=True)
check(not failed_closed(op), "opened a scratch project (project.open discardChanges)")

base = call("project.get_summary").get("trackCount", 0)
bed = add_track("MCP SL bed 7.1.2")
call("spatial.set_track_channels", track=bed, channels=10)
o1 = add_track("Object 1")
o2 = add_track("Object 2")
o3 = add_track("Object 3")
renderer = add_track("MCP SL Renderer Bus")
OBJ = [o1, o2, o3]

# ================================================================================================
print("\n== 1. spatial.orchestrate_sends — dryRun plan (empty bus) ==")
dry = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
           objectTracks=OBJ, dryRun=True)
check(dry.get("dryRun") is True and dry.get("bedChannels") == 10 and dry.get("objectCount") == 3 and
      dry.get("totalChannels") == 13,
      "dryRun: 10-ch bed + 3 objects = 13 renderer channels")
inputs = {i.get("rendererChannel"): (i.get("kind"), i.get("sourceTrack"), i.get("width"))
          for i in dry.get("inputs", [])}
check(inputs.get(1) == ("bed", bed, 10), "roster: bed on renderer channel 1, width 10",
      "ch1=%s" % (inputs.get(1),))
check(inputs.get(11) == ("object", o1, 1) and inputs.get(12) == ("object", o2, 1) and
      inputs.get(13) == ("object", o3, 1),
      "roster: objects on renderer channels 11/12/13 (bed-first), mono")
acts = {a.get("action") for a in dry.get("actions", [])}
check(acts == {"add"} and dry.get("counts", {}).get("add") == 4,
      "empty bus -> 4 adds, nothing to reuse", "counts=%s" % dry.get("counts"))

# ================================================================================================
print("\n== 2. spatial.orchestrate_sends — apply ==")
ap = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
          objectTracks=OBJ, dryRun=False)
check(ap.get("ok") is True and ap.get("dryRun") is False and ap.get("counts", {}).get("add") == 4,
      "apply wired 4 sends (bed + 3 objects)", "counts=%s" % ap.get("counts"))

insp = call("analysis.send_layout_inspect", rendererTrack=renderer, bedLayout="7.1.2", expectedObjects=3)
check(insp.get("ok") is True and insp.get("bedPresent") is True and insp.get("objectChannels") == 3 and
      insp.get("coveredChannels") == 13,
      "inspect: clean layout (bed present, 3 objects, 13 channels used)",
      "issues=%s" % insp.get("issues"))
check(insp.get("busChannels", 0) >= 13, "renderer bus widened to >= 13 channels",
      "busChannels=%s" % insp.get("busChannels"))
byc = recv_by_channel(insp)
check(byc.get(1) == (bed, 10), "inspect receive: bed at channel 1 width 10", "ch1=%s" % (byc.get(1),))
check(byc.get(11) == (o1, 1) and byc.get(12) == (o2, 1) and byc.get(13) == (o3, 1),
      "inspect receives: objects at channels 11/12/13 width 1")

# ================================================================================================
print("\n== 3. idempotency — re-apply reuses, never duplicates ==")
recvCount1 = call("send.list", track=renderer, category="receive").get("sendCount")
re_ap = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
             objectTracks=OBJ, dryRun=False)
c = re_ap.get("counts", {})
check(c.get("reuse") == 4 and c.get("add") == 0 and c.get("fix") == 0 and c.get("stray") == 0,
      "re-run is idempotent: 4 reuse, 0 add/fix/stray", "counts=%s" % c)
recvCount2 = call("send.list", track=renderer, category="receive").get("sendCount")
check(recvCount1 == recvCount2 == 4, "renderer receive count unchanged (4) — no duplicate sends",
      "before=%s after=%s" % (recvCount1, recvCount2))
insp2 = call("analysis.send_layout_inspect", rendererTrack=renderer, bedLayout="7.1.2", expectedObjects=3)
check(insp2.get("ok") is True, "inspect still clean after re-apply", "issues=%s" % insp2.get("issues"))

# ================================================================================================
print("\n== 4. auto-discovery of 'Object N' tracks (no objectTracks given) ==")
disc_run = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
                dryRun=True)
check(disc_run.get("discovered") is True and disc_run.get("objectCount") == 3,
      "auto-discovered 3 'Object N' tracks", "objectCount=%s discovered=%s"
      % (disc_run.get("objectCount"), disc_run.get("discovered")))
di = {i.get("rendererChannel"): i.get("sourceTrack") for i in disc_run.get("inputs", [])}
check(di.get(11) == o1 and di.get(12) == o2 and di.get(13) == o3,
      "discovered objects rostered in N order (Object 1->ch11, 2->12, 3->13)")

# ================================================================================================
print("\n== 5. inspect flags unrouted objects ==")
under = call("analysis.send_layout_inspect", rendererTrack=renderer, bedLayout="7.1.2", expectedObjects=5)
check(under.get("ok") is False and "unrouted" in issue_codes(under),
      "declaring 5 objects but routing 3 -> unrouted", "issues=%s" % under.get("issues"))

# ================================================================================================
print("\n== 6. stray detection + prune ==")
stray = add_track("MCP SL Stray")
call("send.add", track=stray, dest=renderer)  # a send with no roster slot
plan = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
            objectTracks=OBJ, dryRun=True)
check(plan.get("counts", {}).get("stray") == 1 and len(plan.get("strays", [])) == 1,
      "the extra send is reported as a stray", "counts=%s" % plan.get("counts"))
check(plan.get("strays", [{}])[0].get("action") == "kept",
      "stray is KEPT by default (non-destructive)")
pr = call("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
          objectTracks=OBJ, prune=True, dryRun=False)
cp = pr.get("counts", {})
check(cp.get("reuse") == 4 and cp.get("pruned") == 1,
      "prune=true removes the stray, reuses the 4 roster sends", "counts=%s" % cp)
recvCount3 = call("send.list", track=renderer, category="receive").get("sendCount")
check(recvCount3 == 4, "renderer back to 4 receives after prune", "count=%s" % recvCount3)
insp3 = call("analysis.send_layout_inspect", rendererTrack=renderer, bedLayout="7.1.2", expectedObjects=3)
check(insp3.get("ok") is True, "inspect clean again after prune", "issues=%s" % insp3.get("issues"))

# ================================================================================================
print("\n== 7. fail closed on the Dolby master limits ==")
over = raw("spatial.orchestrate_sends", rendererTrack=renderer, bedTrack=bed, bedLayout="7.1.2",
           objectTracks=[o1] * 119, dryRun=True)
check(failed_closed(over) and err_code(over) == "constraint_violation",
      "119 objects -> fails closed (constraint_violation / over_118)", "error=%s" % err_code(over))

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for t in sorted({t for t in (bed, o1, o2, o3, renderer, stray) if isinstance(t, int)}, reverse=True):
        call("track.remove", track=t)
    now = call("project.get_summary").get("trackCount")
    check(now == base, "all scratch tracks removed", "count=%s base=%s" % (now, base))
else:
    notes.append("scratch project + tracks left in place (pass --cleanup to remove the tracks).")

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
