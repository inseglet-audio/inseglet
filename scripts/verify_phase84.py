#!/usr/bin/env python3
"""Live-verify Phase 8 Batch P8-4 — depth fill (envelope/MIDI depth, rec-mode, freeze, metering,
master-FX remover).

Drives every P8-4 tool against a running REAPER + the loaded extension, proving the SDK path (not
just the host stub) works end-to-end. The gate builds ONE scratch track (+ a 2s MIDI item on it),
exercises the tools on it, and (with --cleanup) removes it.

SAFETY / pump-hygiene (the App Nap invariant):
  * Nothing here opens a dialog. The freeze leg uses REAPER's no-dialog freeze/unfreeze actions on
    the SCRATCH track only (a 2-second MIDI item + one ReaSynth — the stock synth gives the render a
    real audio source) — the render is synchronous and sub-second; the tool isolates track selection
    and restores it.
  * Automation mode and record mode are restored to their pre-test values.
  * The master-FX leg adds ONE ReaEQ to the master chain and then removes it with master.remove_fx
    (net-zero on the master chain — this also exercises the P8-3 leftover-FX cleanup path).

REAPER must be in the FOREGROUND. Stdlib only.

Usage:
    python3 verify_phase84.py                 # auto-find reaper_mcp.json; leaves the scratch track
    python3 verify_phase84.py --cleanup       # also delete the scratch track it created
    python3 verify_phase84.py /path/to/reaper_mcp.json
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
EXPECTED_SURFACE = 138  # 125 (D3 baseline) + 13 (P8-4)

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
    with urllib.request.urlopen(req, timeout=60) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def raw(tool_name, **arguments):
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_phase84", "version": "0"}})

passed = failed = skipped = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

# ================================================================================================
initialize()
print(f"== 0. surface == {EXPECTED_SURFACE} ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      f"tool surface == {EXPECTED_SURFACE} (125 + 13 P8-4)", f"count={enum.get('count')}")
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
p84 = ["envelope.get_points", "envelope.delete_point", "envelope.clear_range",
       "envelope.set_automation_mode", "midi.set_note", "midi.set_cc", "midi.insert_notes",
       "midi.select_notes", "track.set_rec_mode", "track.freeze", "track.unfreeze",
       "track.get_peak", "master.remove_fx"]
missing = [t for t in p84 if t not in listed]
check(not missing, "all 13 P8-4 tools present in tools/list", f"missing={missing}" if missing else "")

# --- scratch setup: one track at the end of the project ---
base = call("project.get_summary").get("trackCount", 0)
track = call("track.add", index=base, name="MCP P8-4 scratch").get("trackIndex", base)
print(f"\nscratch track: {track}\n")

print("== 1. envelope depth (add_point autoActivate / get_points / delete_point / clear_range) ==")
# The scratch track is fresh, so its Volume envelope is NOT active — autoActivate must create it.
ap = call("envelope.add_point", track=track, envelope="Volume", time=0.5, value=1.0,
          autoActivate=True)
check(ap.get("ok") is True and ap.get("activated") is True,
      "add_point(autoActivate) activates the Volume envelope", f"activated={ap.get('activated')}")
ap2 = call("envelope.add_point", track=track, envelope="Volume", time=1.5, value=0.5)
check(ap2.get("ok") is True and ap2.get("activated") is False,
      "second add_point needs no activation", f"pointCount={ap2.get('pointCount')}")
evl = call("envelope.list", track=track)
check(any(e.get("name") == "Volume" for e in evl.get("envelopes", [])),
      "envelope.list shows the activated Volume envelope")
gp = call("envelope.get_points", track=track, envelope="Volume")
times = [round(p["time"], 3) for p in gp.get("points", [])]
check(0.5 in times and 1.5 in times,
      "get_points returns both inserted points", f"times={times}")
gpw = call("envelope.get_points", track=track, envelope="Volume", start=1.0, end=2.0)
check(all(1.0 <= p["time"] <= 2.0 for p in gpw.get("points", [])) and len(gpw.get("points", [])) >= 1,
      "get_points honors the [start,end] window", f"n={len(gpw.get('points', []))}")
n_before = gp.get("pointCount", 0)
# Delete the point we know sits at 1.5 (look its index up from the windowed read).
del_idx = gpw["points"][0]["index"]
dp = call("envelope.delete_point", track=track, envelope="Volume", index=del_idx)
check(dp.get("ok") is True and dp.get("pointCount", 99) == n_before - 1,
      "delete_point removes one point", f"{n_before} -> {dp.get('pointCount')}")
# clear_range contract = [start,end): add three points, clear a window that covers them AND the
# earlier 0.5 point but NOT the activation seed at t=0 — the seed must survive, the rest must go.
for tt in (1.0, 2.0, 3.0):
    call("envelope.add_point", track=track, envelope="Volume", time=tt, value=0.8)
cr = call("envelope.clear_range", track=track, envelope="Volume", start=0.4, end=3.5)
left = call("envelope.get_points", track=track, envelope="Volume", start=0.4, end=3.5)
check(cr.get("ok") is True and len(left.get("points", [])) == 0,
      "clear_range removes every point in [start,end)",
      f"leftInRange={len(left.get('points', []))} totalAfter={cr.get('pointCount')}")
check(cr.get("pointCount", 99) == 1,
      "the activation seed (t=0, outside the range) survives", f"pointCount={cr.get('pointCount')}")

print("\n== 2. automation mode (envelope.set_automation_mode <-> track.get) ==")
mode0 = call("track.get", track=track).get("automationMode", 0)
sm = call("envelope.set_automation_mode", track=track, mode=2)
check(sm.get("ok") is True and sm.get("mode") == 2 and sm.get("previousMode") == mode0,
      "set_automation_mode(touch) echoes mode + previousMode",
      f"prev={sm.get('previousMode')} mode={sm.get('mode')}")
check(call("track.get", track=track).get("automationMode") == 2,
      "track.get reflects automationMode=2")
call("envelope.set_automation_mode", track=track, mode=mode0)  # restore
check(call("track.get", track=track).get("automationMode") == mode0,
      f"automation mode restored to {mode0}")

print("\n== 3. MIDI depth (insert_notes batch / set_note / select_notes / set_cc) ==")
item = call("midi.create_item", track=track, position=0.0, length=2.0, name="P8-4 scratch").get("item", 0)
ins = call("midi.insert_notes", track=track, item=item, notes=[
    {"startBeats": 0.0, "pitch": 60, "velocity": 90},
    {"startBeats": 1.0, "pitch": 64, "velocity": 90},
    {"startBeats": 2.0, "pitch": 67, "velocity": 90}])
check(ins.get("ok") is True and ins.get("inserted") == 3 and ins.get("noteCount") == 3,
      "insert_notes batch of 3", f"inserted={ins.get('inserted')} noteCount={ins.get('noteCount')}")
sn = call("midi.set_note", track=track, item=item, index=0, pitch=62, velocity=120)
check(sn.get("ok") is True, "set_note edits pitch+velocity in place")
nl = call("midi.list_notes", track=track, item=item).get("notes", [])
n0 = next((n for n in nl if round(n["startBeats"], 3) == 0.0), {})
check(n0.get("pitch") == 62 and n0.get("velocity") == 120,
      "list_notes confirms the in-place edit", f"pitch={n0.get('pitch')} vel={n0.get('velocity')}")
sn2 = call("midi.set_note", track=track, item=item, index=0, startBeats=3.0)
nl2 = call("midi.list_notes", track=track, item=item).get("notes", [])
check(any(round(n["startBeats"], 3) == 3.0 and n["pitch"] == 62 for n in nl2),
      "set_note moves a note in time (re-sorted)", f"starts={[round(n['startBeats'],2) for n in nl2]}")
sel = call("midi.select_notes", track=track, item=item, selected=True)
check(sel.get("matched") == 3, "select_notes (no filter) selects all", f"matched={sel.get('matched')}")
self_ = call("midi.select_notes", track=track, item=item, selected=False, pitchMin=62, pitchMax=62)
check(self_.get("matched") == 1, "select_notes pitch filter matches 1", f"matched={self_.get('matched')}")
nl3 = call("midi.list_notes", track=track, item=item).get("notes", [])
check(sum(1 for n in nl3 if n["selected"]) == 2,
      "selection state: 3 selected minus 1 deselected == 2")
call("midi.select_notes", track=track, item=item, selected=False)  # deselect all
cc = call("midi.insert_cc", track=track, item=item, beats=0.0, cc=1, value=10)
check(cc.get("ok") is True, "insert_cc (mod wheel) ok")
sc = call("midi.set_cc", track=track, item=item, index=0, value=99)
check(sc.get("ok") is True, "set_cc edits in place")
cl = call("midi.list_cc", track=track, item=item).get("events", [])
check(len(cl) == 1 and cl[0].get("value") == 99,
      "list_cc confirms the CC value edit", f"value={cl[0].get('value') if cl else None}")

print("\n== 4. record mode (track.set_rec_mode <-> track.get) ==")
rec0 = call("track.get", track=track).get("recMode", 0)
rm = call("track.set_rec_mode", track=track, mode=2)
check(rm.get("ok") is True and rm.get("mode") == 2, "set_rec_mode(2 none/monitor-only) echoes")
check(call("track.get", track=track).get("recMode") == 2, "track.get reflects recMode=2")
call("track.set_rec_mode", track=track, mode=rec0)  # restore
check(call("track.get", track=track).get("recMode") == rec0, f"rec mode restored to {rec0}")

print("\n== 5. freeze / unfreeze (scratch track: 2s MIDI item + ReaSynth) ==")
# ReaSynth (stock, always installed) gives the MIDI item an actual AUDIO source — freezing a
# synth-less MIDI track renders nothing and REAPER creates NO frozen item (live-gate finding).
call("fx.add", track=track, name="ReaSynth")
fz = call("track.freeze", track=track, mode="stereo")
check(fz.get("ok") is True and fz.get("fxCount") == 0,
      "freeze -> ok, online FX moved into the freeze (fxCount 1->0)", f"fxCount={fz.get('fxCount')}")
check(fz.get("itemCount") == 1,
      "freeze rendered exactly one item", f"itemCount={fz.get('itemCount')}")
if fz.get("itemCount", 0) >= 1:
    tl = call("take.list", track=track, item=0)
    act = next((t for t in tl.get("takes", []) if t.get("active")), {})
    check(act.get("isMidi") is False,
          "frozen item's active take is rendered AUDIO (not MIDI)", f"isMidi={act.get('isMidi')}")
else:
    check(False, "frozen item's active take is rendered AUDIO (not MIDI)", "no item to inspect")
uf = call("track.unfreeze", track=track)
check(uf.get("ok") is True and uf.get("fxCount") == 1 and uf.get("itemCount", 0) >= 1,
      "unfreeze -> FX restored (fxCount 0->1), items intact",
      f"fxCount={uf.get('fxCount')} itemCount={uf.get('itemCount')}")
if uf.get("itemCount", 0) >= 1:
    tl2 = call("take.list", track=track, item=0)
    act2 = next((t for t in tl2.get("takes", []) if t.get("active")), {})
    check(act2.get("isMidi") is True,
          "unfrozen item's active take is MIDI again", f"isMidi={act2.get('isMidi')}")
else:
    check(False, "unfrozen item's active take is MIDI again", "no item to inspect")

print("\n== 6. metering (track.get_peak) ==")
pk = call("track.get_peak", track=track)
check(pk.get("channels", 0) >= 2 and len(pk.get("peaks", [])) == pk.get("channels"),
      "get_peak returns one entry per track channel", f"channels={pk.get('channels')}")
check(all(("peak" in p and "peakDb" in p) for p in pk.get("peaks", [])),
      "each peak entry carries linear + dB")
pk1 = call("track.get_peak", track=track, channel=0)
check(len(pk1.get("peaks", [])) == 1 and pk1["peaks"][0]["channel"] == 0,
      "get_peak(channel=0) returns exactly that channel")
notes.append("peaks read ~-inf on a stopped transport — meaningful values need playback/monitoring")

print("\n== 7. master.remove_fx (net-zero master chain) ==")
m0 = call("master.get").get("fxCount", 0)
add = call("master.add_fx", name="ReaEQ")
midx = add.get("fxIndex", m0)
check(call("master.get").get("fxCount") == m0 + 1, "master.add_fx -> fxCount+1")
rm_ = call("master.remove_fx", fx=midx)
check(rm_.get("ok") is True and rm_.get("fxCount") == m0,
      f"master.remove_fx -> fxCount back to {m0}", f"fxCount={rm_.get('fxCount')}")
bad = raw("master.remove_fx", fx=99)
check(bad.get("isError") is True, "master.remove_fx out-of-range -> isError")

# --- cleanup ---
if DO_CLEANUP:
    print("\n== cleanup ==")
    call("track.remove", track=track)
    check(call("project.get_summary").get("trackCount") == base, "scratch track removed")
else:
    notes.append(f"scratch track {track} left in place (pass --cleanup to remove)")

print(f"\n{'='*64}\n  PASS={passed}  FAIL={failed}  SKIP={skipped}")
for n in notes:
    print(f"  note: {n}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
