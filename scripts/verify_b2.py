#!/usr/bin/env python3
"""Live-verify Batch B2 breadth (20 tools) against a running REAPER + extension.

Groove/quantize (9, midi.*), takes/comping (8), transport extras (3). Proves, end-to-end on the real
SDK path, that each tool produces the intended effect — not just that it is registered. The groove math
itself is proven deterministically off-REAPER in unit.groove; this gate drives the tools live and reads
the result back through midi.list_notes / take.list / transport.get_state.

REAPER must be in the FOREGROUND, and `cmake --install build` must have shipped the current dylib.
Nothing opens a dialog. Stdlib only.

Usage:
    python3 verify_b2.py                 # auto-find reaper_mcp.json; leaves the scratch tracks
    python3 verify_b2.py --cleanup       # also delete the scratch tracks it created
    python3 verify_b2.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 181  # 161 + 20 (Batch B2 breadth)
B2 = ["midi.quantize", "midi.humanize", "midi.apply_groove", "midi.extract_groove", "midi.transpose",
      "midi.scale_velocity", "midi.legato", "midi.nudge", "midi.stretch",
      "take.add", "take.delete", "take.crop_to_active", "take.set_vol", "take.set_pan",
      "take.set_pitch", "take.set_playrate", "items.implode_to_takes",
      "transport.set_repeat", "transport.set_playrate", "transport.set_metronome"]

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
    with urllib.request.urlopen(req, timeout=120) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError("%s -> %s" % (method, j["error"]))
    return j["result"]

def call(tool, **arguments):
    r = rpc("tools/call", {"name": tool, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError("%s isError: %s" % (tool, r.get("content")))
    return r.get("structuredContent", {})

def raw(tool, **arguments):
    return rpc("tools/call", {"name": tool, "arguments": arguments})

passed = failed = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def near(a, b, tol=0.02):
    return a is not None and abs(a - b) <= tol

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_b2", "version": "0"}})

def add_track(name):
    idx = call("project.get_summary").get("trackCount", 0)
    call("track.add", index=idx, name=name)
    return idx

def note_starts(track, item):
    ns = call("midi.list_notes", track=track, item=item).get("notes", [])
    return sorted(n["startBeats"] for n in ns)

def make_midi(track, starts, pitch=60, vel=100, length=0.2):
    it = call("midi.create_item", track=track, position=0.0, length=8.0).get("item", 0)
    call("midi.insert_notes", track=track, item=it,
         notes=[{"startBeats": s, "pitch": pitch, "velocity": vel, "lengthBeats": length} for s in starts])
    return it

# ================================================================================================
initialize()
print("== 0. surface + presence ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE, "tool surface == %d (161 + 20 B2)" % EXPECTED_SURFACE,
      "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
missing = [n for n in B2 if n not in listed]
check(not missing, "all 20 B2 tools present in tools/list", "missing=%s" % missing)

base = call("project.get_summary").get("trackCount", 0)
S = add_track("MCP B2 scratch")

# ================================================================================================
print("\n== 1. transport extras ==")
call("transport.set_repeat", enabled=True)
check(call("transport.get_state").get("loop") is True, "set_repeat(true) -> loop on")
call("transport.set_repeat", enabled=False)
check(call("transport.get_state").get("loop") is False, "set_repeat(false) -> loop off")

pr = call("transport.set_playrate", rate=1.5)
check(near(pr.get("rate"), 1.5), "set_playrate(1.5) reads back 1.5", "rate=%s" % pr.get("rate"))
call("transport.set_playrate", rate=1.0)

m1 = call("transport.set_metronome", enabled=True)
m0 = call("transport.set_metronome", enabled=False)
check(m1.get("enabled") is True and m0.get("enabled") is False,
      "set_metronome true/false toggles the click state", "on=%s off=%s" % (m1, m0))

# ================================================================================================
print("\n== 2. groove / quantize (midi.*) ==")
# quantize: off-grid starts snap to the 1/2-note grid
it = make_midi(S, [0.05, 0.53, 1.02, 1.55])
call("midi.quantize", track=S, item=it, grid=0.5, strength=1.0)
qs = note_starts(S, it)
check(all(near(a, b) for a, b in zip(qs, [0.0, 0.5, 1.0, 1.5])), "quantize snaps starts to the grid",
      "starts=%s" % [round(x, 3) for x in qs])

# transpose +12, then an out-of-range transpose is fully skipped
it = make_midi(S, [0.0, 1.0], pitch=60)
call("midi.transpose", track=S, item=it, semitones=12)
p = [n["pitch"] for n in call("midi.list_notes", track=S, item=it).get("notes", [])]
check(p == [72, 72], "transpose +12 -> pitch 72", "pitch=%s" % p)
tr = call("midi.transpose", track=S, item=it, semitones=127)
check(tr.get("moved") == 0 and tr.get("skipped") == 2, "transpose out of range is skipped, not clamped",
      "moved=%s skipped=%s" % (tr.get("moved"), tr.get("skipped")))

# scale_velocity compresses toward the center
it = make_midi(S, [0.0], vel=100)
call("midi.scale_velocity", track=S, item=it, scale=0.5, center=64)
v = [n["velocity"] for n in call("midi.list_notes", track=S, item=it).get("notes", [])]
check(v == [82], "scale_velocity 0.5 about 64: 100 -> 82", "vel=%s" % v)

# nudge shifts starts
it = make_midi(S, [0.0, 0.5])
call("midi.nudge", track=S, item=it, beats=0.25)
check(all(near(a, b) for a, b in zip(note_starts(S, it), [0.25, 0.75])), "nudge +0.25 shifts starts")

# stretch x2 about anchor 0
it = make_midi(S, [0.0, 1.0])
call("midi.stretch", track=S, item=it, factor=2.0, anchorBeats=0.0)
check(all(near(a, b) for a, b in zip(note_starts(S, it), [0.0, 2.0])), "stretch x2: beat 1 -> beat 2")

# legato closes the gap to the next note start
it = make_midi(S, [0.0, 1.0, 2.0], length=0.1)
call("midi.legato", track=S, item=it, gap=0.0)
lens = [round(n["lengthBeats"], 3) for n in call("midi.list_notes", track=S, item=it).get("notes", [])]
check(near(lens[0], 1.0) and near(lens[1], 1.0), "legato extends notes to the next start", "lens=%s" % lens)

# humanize is deterministic given a seed (changed count == note count)
it = make_midi(S, [0.0, 0.5, 1.0, 1.5])
hz = call("midi.humanize", track=S, item=it, timing=0.02, velocity=8, seed=1)
check(hz.get("changed") == 4, "humanize touches every note", "changed=%s" % hz.get("changed"))

# extract a groove from a swung take, then apply it
src = make_midi(S, [0.0, 0.30, 0.5, 0.80])          # odd 1/16 slots ~0.05 late
g = call("midi.extract_groove", track=S, item=src, grid=0.25, steps=2)
tmpl = g.get("template", [])
check(len(tmpl) == 2 and tmpl[1]["timeOffset"] > 0.02, "extract_groove recovers the off-beat push",
      "template=%s" % tmpl)
dst = make_midi(S, [0.0, 0.25, 0.5, 0.75])          # straight
ag = call("midi.apply_groove", track=S, item=dst, grid=0.25, template=tmpl, strength=1.0)
ds = note_starts(S, dst)
check(ag.get("changed") == 4 and ds[1] > 0.26, "apply_groove pushes off-beat notes later",
      "starts=%s" % [round(x, 3) for x in ds])
# named swing groove
it = make_midi(S, [0.0, 0.5, 1.0, 1.5])
call("midi.apply_groove", track=S, item=it, groove="swing8", amount=0.5)
sw = note_starts(S, it)
check(sw[1] > 0.6, "named swing8 delays the off-beats", "starts=%s" % [round(x, 3) for x in sw])

# ================================================================================================
print("\n== 3. takes / comping ==")
# a MIDI item starts with one take; add two empty takes
it = call("midi.create_item", track=S, position=0.0, length=4.0).get("item", 0)
c0 = call("take.list", track=S, item=it).get("takeCount", 0)
a1 = call("take.add", track=S, item=it, name="comp B")
a2 = call("take.add", track=S, item=it, name="comp C")
check(a2.get("takeCount") == c0 + 2, "take.add appends takes", "count %s -> %s" % (c0, a2.get("takeCount")))

# per-take audio properties on the active take
sv = call("take.set_vol", track=S, item=it, db=-6.0)
sp = call("take.set_pan", track=S, item=it, pan=0.5)
spi = call("take.set_pitch", track=S, item=it, semitones=2.0)
srt = call("take.set_playrate", track=S, item=it, rate=2.0)
check(near(sv.get("db"), -6.0, 0.1), "take.set_vol -6 dB", "db=%s" % sv.get("db"))
check(near(sp.get("pan"), 0.5, 0.001), "take.set_pan 0.5", "pan=%s" % sp.get("pan"))
check(near(spi.get("semitones"), 2.0, 0.001), "take.set_pitch +2", "semis=%s" % spi.get("semitones"))
check(near(srt.get("rate"), 2.0, 0.001), "take.set_playrate 2.0", "rate=%s" % srt.get("rate"))

# delete a take, then crop to the active take
d = call("take.delete", track=S, item=it, take=a2.get("take"))
check(d.get("takeCount") == c0 + 1, "take.delete removes a take", "count=%s" % d.get("takeCount"))
# refuse to delete the only take
cr = call("take.crop_to_active", track=S, item=it)
check(cr.get("takeCount") == 1, "crop_to_active leaves exactly one take", "count=%s" % cr.get("takeCount"))
only = raw("take.delete", track=S, item=it)
check(only.get("isError") is True, "take.delete refuses the only take (use item.delete)")

# implode items across two tracks into a single multi-take comp item
A = add_track("MCP B2 comp A")
B = add_track("MCP B2 comp B")
call("midi.create_item", track=A, position=0.0, length=4.0)
call("midi.create_item", track=B, position=0.0, length=4.0)
imp = call("items.implode_to_takes", items=[{"track": A, "item": 0}, {"track": B, "item": 0}])
check(imp.get("takeCount", 0) >= 2, "implode_to_takes yields a >=2-take comp item",
      "selected=%s takeCount=%s" % (imp.get("selectedItems"), imp.get("takeCount")))

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    # remove scratch tracks from the highest index down so earlier indices stay valid
    for t in sorted([S, A, B], reverse=True):
        try:
            call("track.remove", track=t)
        except Exception as e:
            notes.append("cleanup of track %d skipped (%s)" % (t, str(e)[:60]))
    now = call("project.get_summary").get("trackCount", -1)
    check(now == base, "scratch tracks removed", "trackCount=%s base=%s" % (now, base))
else:
    notes.append("scratch tracks %s left in place (pass --cleanup to remove)" % [S, "A/B comp"])

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
