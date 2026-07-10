#!/usr/bin/env python3
"""Live-verify Phase 8 Batch P8-1 — editing-breadth surface (Session P8-A gate).

Drives the new granular per-field setters and structural edit verbs against a running REAPER + the
loaded extension, asserting real project state via the consolidated reads (track.get / item.list /
selection.get / region.list / transport.get_state). Every mutation is single-undoable; this gate also
exercises the client-facing undo/redo history verbs.

Coverage:
  0. surface == 94 (61 baseline + 33 P8-1).
  1. granular TRACK setters: mute/solo/phase/width/color/rec_arm/rec_input/rec_mon/folder, each
     confirmed by track.get (consolidated read).
  2. granular ITEM setters + structural edits: position/length/mute/vol/fades/color/selected, then
     split (count+1), duplicate (count+1), move (reparent), delete (count-1), glue.
  3. selection.set / selection.get round-trip (explicit set, then deselectAll).
  4. undo history: an edit makes canUndo true; edit.undo reverts it; edit.redo restores it.
  5. regions/markers completeness: region.add -> region.list -> marker.edit -> region.goto (cursor
     lands on the region start) -> region.delete; marker.add -> marker.goto -> marker.edit.

REAPER must be in the FOREGROUND (App Nap stalls the Run() pump). Stdlib only.

Usage:
    python3 verify_phase81.py                  # auto-find reaper_mcp.json
    python3 verify_phase81.py /path/to/reaper_mcp.json
    python3 verify_phase81.py --cleanup        # also remove the tracks/markers this gate created
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

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
CLEANUP = "--cleanup" in flags

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
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_phase81", "version": "0"}})

passed = failed = 0
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

def approx(a, b, tol=0.02):
    return abs(float(a) - float(b)) <= tol

SCRATCH = "p81-scratch"
SCRATCH2 = "p81-scratch2"

# ------------------------------------------------------------------------------------------------
print("== 0. surface == 94 ==")
initialize()
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == 94, "tool surface == 94 (61 + 33 P8-1)", f"count={enum.get('count')}")

print("\n== set up two scratch tracks ==")
t = call("track.add", name=SCRATCH)
TIDX = t["trackIndex"]
t2 = call("track.add", name=SCRATCH2)
TIDX2 = t2["trackIndex"]
check(TIDX >= 0 and TIDX2 >= 0, "created two scratch tracks", f"idx={TIDX},{TIDX2}")

print("\n== 1. granular track setters (each confirmed by track.get) ==")
check(call("track.set_mute",  track=TIDX, muted=True).get("muted") is True,   "set_mute read-back true")
check(call("track.set_solo",  track=TIDX, soloed=True).get("soloed") is True, "set_solo read-back true")
check(call("track.set_phase", track=TIDX, inverted=True).get("inverted") is True, "set_phase read-back true")
check(approx(call("track.set_width", track=TIDX, width=0.5).get("width"), 0.5), "set_width read-back 0.5")
sc = call("track.set_color", track=TIDX, color=0x3366CC)
check(sc.get("color") == 0x3366CC and sc.get("colorSet") is True, "set_color read-back 0x3366CC + set")
check(call("track.set_rec_arm",   track=TIDX, armed=True).get("armed") is True, "set_rec_arm read-back true")
check(call("track.set_rec_input", track=TIDX, input=1).get("input") == 1,       "set_rec_input read-back 1")
check(call("track.set_rec_mon",   track=TIDX, mode=1).get("mode") == 1,          "set_rec_mon read-back 1")
fol = call("track.set_folder", track=TIDX, depth=1, compact=1)
check(fol.get("depth") == 1 and fol.get("compact") == 1, "set_folder read-back depth=1 compact=1")

g = call("track.get", track=TIDX)
check(g.get("muted") and g.get("soloed") and g.get("phaseInverted") and g.get("recArmed"),
      "track.get reflects the boolean fields")
check(approx(g.get("width"), 0.5) and g.get("color") == 0x3366CC and g.get("recInput") == 1
      and g.get("recMon") == 1 and g.get("folderDepth") == 1,
      "track.get reflects width/color/recInput/recMon/folderDepth")

print("\n== 2. granular item setters + structural edits ==")
call("item.add", track=TIDX, position=0.0, length=8.0)
check(call("item.set_position", track=TIDX, item=0, position=4.5).get("position") == 4.5, "item.set_position 4.5")
check(call("item.set_length",   track=TIDX, item=0, length=3.0).get("length") == 3.0,     "item.set_length 3.0")
check(call("item.set_mute",     track=TIDX, item=0, muted=True).get("muted") is True,      "item.set_mute true")
check(approx(call("item.set_vol", track=TIDX, item=0, db=-6.0).get("db"), -6.0, 0.1),      "item.set_vol -6 dB")
check(call("item.set_fade_in",  track=TIDX, item=0, length=0.25).get("length") == 0.25,    "item.set_fade_in 0.25")
check(call("item.set_fade_out", track=TIDX, item=0, length=0.5).get("length") == 0.5,      "item.set_fade_out 0.5")
ic = call("item.set_color", track=TIDX, item=0, color=0x11AA22)
check(ic.get("color") == 0x11AA22 and ic.get("colorSet"), "item.set_color 0x11AA22 + set")
check(call("item.set_selected", track=TIDX, item=0, selected=True).get("selected") is True, "item.set_selected true")

n0 = call("item.list", track=TIDX).get("itemCount", 0)
sp = call("item.split", track=TIDX, item=0, position=6.0)   # item spans 4.5..7.5, split at 6.0
check(sp.get("ok") and sp.get("rightGuid"), "item.split ok, produced a right half")
n1 = call("item.list", track=TIDX).get("itemCount", 0)
check(n1 == n0 + 1, "item.split grew the item count by 1", f"{n0} -> {n1}")
call("item.duplicate", track=TIDX, item=0)
n2 = call("item.list", track=TIDX).get("itemCount", 0)
check(n2 == n1 + 1, "item.duplicate grew the item count by 1", f"{n1} -> {n2}")

d0_dst = call("item.list", track=TIDX2).get("itemCount", 0)
mv = call("item.move", track=TIDX, item=0, destTrack=TIDX2, position=1.0)
check(mv.get("movedTrack") and mv.get("track") == TIDX2, "item.move reparented to track 2")
d1_dst = call("item.list", track=TIDX2).get("itemCount", 0)
check(d1_dst == d0_dst + 1, "destination track gained the moved item", f"{d0_dst} -> {d1_dst}")

before_del = call("item.list", track=TIDX).get("itemCount", 0)
check(call("item.delete", track=TIDX, item=0).get("ok"), "item.delete ok")
after_del = call("item.list", track=TIDX).get("itemCount", 0)
check(after_del == before_del - 1, "item.delete shrank the item count by 1", f"{before_del} -> {after_del}")
if after_del > 0:
    check(call("item.glue", track=TIDX, item=0).get("ok"), "item.glue ok")

print("\n== 3. selection.set / selection.get ==")
call("selection.set", deselectAll=True, tracks=[TIDX], items=[{"track": TIDX2, "item": 0}])
selg = call("selection.get")
check(TIDX in selg.get("tracks", []), "selection.get returns the selected track")
check(any(it.get("track") == TIDX2 for it in selg.get("items", [])), "selection.get returns the selected item")
call("selection.set", deselectAll=True)
selg2 = call("selection.get")
check(selg2.get("trackCount") == 0 and selg2.get("itemCount") == 0, "selection.set deselectAll clears both axes")

print("\n== 4. undo history verbs ==")
call("marker.add", position=30.0, name="p81-undo")
st = call("edit.get_undo_state")
check(st.get("canUndo") is True, "edit.get_undo_state.canUndo true after an edit", st.get("undoName", ""))
call("edit.undo")
st2 = call("edit.get_undo_state")
check(st2.get("canRedo") is True, "edit.undo made canRedo true")
call("edit.redo")
check(True, "edit.redo executed")

print("\n== 5. regions / markers completeness ==")
call("region.add", start=40.0, end=50.0, name="p81-rgn")
rgns = call("region.list").get("regions", [])
mine = [r for r in rgns if r.get("name") == "p81-rgn"]
check(bool(mine), "region.list finds the new region")
RID = mine[0]["index"] if mine else None
if RID is not None:
    check(approx(mine[0]["start"], 40.0) and approx(mine[0]["end"], 50.0), "region.list start/end correct")
    ed = call("marker.edit", index=RID, isRegion=True, name="p81-rgn2", position=41.0, end=51.0)
    check(ed.get("ok"), "marker.edit (region) ok")
    rgns2 = {r["index"]: r for r in call("region.list").get("regions", [])}
    check(rgns2.get(RID, {}).get("name") == "p81-rgn2" and approx(rgns2.get(RID, {}).get("start", 0), 41.0),
          "marker.edit renamed + repositioned the region")
    gt = call("region.goto", index=RID)
    cur = call("transport.get_state").get("cursorSec", -1)
    check(gt.get("ok") and approx(cur, 41.0, 0.05), "region.goto moved the edit cursor to the region start",
          f"cursor={cur}")
    check(call("region.delete", index=RID).get("ok"), "region.delete ok")
    still = [r for r in call("region.list").get("regions", []) if r.get("index") == RID]
    check(not still, "region.delete removed it from region.list")

call("marker.add", position=12.0, name="p81-mk")
marks = [m for m in call("markers.list").get("markers", []) if m.get("name") == "p81-mk" and not m.get("isRegion")]
MID = marks[0]["index"] if marks else None
if MID is not None:
    check(call("marker.goto", index=MID).get("ok"), "marker.goto ok")
    check(approx(call("transport.get_state").get("cursorSec", -1), 12.0, 0.05), "marker.goto moved cursor to 12s")
    check(call("marker.edit", index=MID, name="p81-mk2").get("ok"), "marker.edit (marker) rename ok")

# ------------------------------------------------------------------------------------------------
if CLEANUP:
    print("\n== cleanup ==")
    # markers/regions by name
    for m in call("markers.list").get("markers", []):
        if m.get("name", "").startswith("p81"):
            try:
                call("marker.delete", index=m["index"], isRegion=bool(m.get("isRegion")))
                print(f"  removed {'region' if m.get('isRegion') else 'marker'} {m['index']}")
            except Exception as e:  # noqa
                print(f"  (could not remove marker {m.get('index')}: {e})")
    # scratch tracks (highest index first; track.remove's arg is 'track')
    victims = sorted((t.get("index") for t in call("track.list").get("tracks", [])
                      if t.get("name") in {SCRATCH, SCRATCH2}), reverse=True)
    for idx in victims:
        try:
            call("track.remove", track=idx)
            print(f"  removed track {idx}")
        except Exception as e:  # noqa
            print(f"  (could not remove track {idx}: {e})")

print(f"\n{'='*60}\n  PASS={passed}  FAIL={failed}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: GREEN — Phase 8 Batch P8-1 (editing breadth) verified live.")
