#!/usr/bin/env python3
"""Build + verify spatial.set_distance's IEM-RoomEncoder source placement against a running REAPER.

WHAT IT CHECKS
  set_distance places a source at (distanceM, azimuthDeg, elevationDeg) by writing RoomEncoder's
  'source position x/y/z'. RoomEncoder's room frame is the ambiX convention +X=front, +Y=left, +Z=up
  (+az=left), so the CORRECT mapping is X=d*cos(az)cos(el), Y=d*sin(az)cos(el), Z=d*sin(el).

  This script drives an azimuth sweep and reads the 'source position x/y/z' params BACK DIRECTLY
  (via fx.get_param — independent of what the tool echoes), then prints a verdict:

    * NO-OP  — the source params don't change across azimuth. The installed build isn't placing the
               source (e.g. the pre-fix param-name mismatch: it looked up 'source x', RoomEncoder
               names it 'source position x').
    * FIXED  — +az drives Source Y (left); a front (az=0) source sits at the front. Correct.
    * BUGGY  — +az drives Source X (front): front and side are swapped (frame not remapped).

  Run BEFORE the patch -> NO-OP; after `git apply SET_DISTANCE_FIX.patch` + rebuild + cmake --install
  -> FIXED. Reads normalized [0,1] params and shows nominal metres using RoomEncoder's documented
  ranges (Source X,Y = +/-15 m, Z = +/-10 m); 0.5 normalized = room centre.

It builds ONE FOA scratch scene with ONE RoomEncoder and drives it with fx:<index> pinned (so it does
NOT stack duplicate encoders). Leaves the scene in place unless --cleanup. REAPER must be FOREGROUND.
Stdlib only.

Usage:
    python3 verify_set_distance.py                 # auto-find reaper_mcp.json; leaves the scene
    python3 verify_set_distance.py --cleanup       # delete the scratch track afterward
    python3 verify_set_distance.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
DISTANCE_M = 10.0
SWEEP = [(0.0, 0.0), (90.0, 0.0), (-90.0, 0.0), (45.0, 0.0), (0.0, 45.0)]
# RoomEncoder's documented real ranges (metres) for reporting nominal metres from normalized [0,1].
RANGE_M = {"x": (-15.0, 15.0), "y": (-15.0, 15.0), "z": (-10.0, 10.0)}

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found - pass its path, or run the 'reaper_mcp: status' action first.")

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

def err_text(result):
    return " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))

rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "verify_set_distance", "version": "0"}})

listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
if "spatial.set_distance" not in listed:
    sys.exit("spatial.set_distance not in tools/list - is the extension installed?")

# ---- build a clearly-named FOA scratch scene with ONE RoomEncoder ----
base = call("project.get_summary").get("trackCount", 0)
track = call("track.add", index=base, name="SET_DISTANCE VERIFY - FOA scene").get("trackIndex", base)
call("spatial.convert_order", track=track, toOrder=1)          # 4-ch FOA carrier
probe = raw("spatial.set_distance", track=track, distanceM=DISTANCE_M, azimuthDeg=0.0, elevationDeg=0.0)
if probe.get("isError"):
    t = err_text(probe).lower()
    if "room encoder" in t or "roomencoder" in t:
        sys.exit("IEM RoomEncoder is not installed - set_distance fails closed. Install it to verify.")
    sys.exit("set_distance errored: " + err_text(probe))
FX = probe.get("structuredContent", {}).get("fxIndex", 0)      # pin this instance to avoid stacking
print("scratch scene track: %d   RoomEncoder fx index: %d\n" % (track, FX))

# ---- discover the source-position param indices by name ----
np = call("fx.list", track=track)
nparams = next((f.get("numParams", 0) for f in np.get("fx", []) if f.get("index") == FX), 0)
src_idx = {}
for p in range(nparams):
    nm = (call("fx.get_param", track=track, fx=FX, param=p).get("name") or "").lower()
    for ax in ("x", "y", "z"):
        if nm == "source position " + ax:
            src_idx[ax] = p
if len(src_idx) != 3:
    print("WARN: could not name-match all of source position x/y/z; found %s" % src_idx)

def read_src():
    out = {}
    for ax, p in src_idx.items():
        v = call("fx.get_param", track=track, fx=FX, param=p).get("normalized")
        out[ax] = v
    return out

def to_m(ax, n):
    if n is None:
        return None
    lo, hi = RANGE_M[ax]
    return lo + n * (hi - lo)

# ---- azimuth / elevation sweep, reading the params back directly (fx pinned) ----
print("Source position read back from RoomEncoder after each set_distance (d = %.1f m):" % DISTANCE_M)
print("(normalized [0,1]; 0.5 = room centre; nominal metres in parens, X/Y +/-15 m, Z +/-10 m)\n")
hdr = "  az     el  |  X=front            Y=left             Z=up"
print(hdr); print("  " + "-" * (len(hdr) - 2))
rows = {}
for az, el in SWEEP:
    call("spatial.set_distance", track=track, fx=FX, distanceM=DISTANCE_M, azimuthDeg=az, elevationDeg=el)
    s = read_src()
    rows[(az, el)] = s
    def cell(ax):
        n = s.get(ax)
        return "   n/a        " if n is None else "%.3f (%+6.1fm)" % (n, to_m(ax, n))
    print("  %+5.0f  %+5.0f |  %s  %s  %s" % (az, el, cell("x"), cell("y"), cell("z")))

# ---- verdict ----
print("\n" + "=" * 70)
def span(ax):
    vals = [rows[k].get(ax) for k in rows if rows[k].get(ax) is not None]
    return (max(vals) - min(vals)) if len(vals) >= 2 else 0.0

moved = max(span("x"), span("y"), span("z"))
x0 = rows[(0.0, 0.0)].get("x"); y0 = rows[(0.0, 0.0)].get("y")
x9 = rows[(90.0, 0.0)].get("x"); y9 = rows[(90.0, 0.0)].get("y")

if moved < 0.02:
    print("  VERDICT: NO-OP - the source position does not change with azimuth.")
    print("    The installed build is not placing the source (pre-fix param-name mismatch:")
    print("    it looks up 'source x' but RoomEncoder names it 'source position x').")
    print("    -> apply SET_DISTANCE_FIX.patch, rebuild + cmake --install, re-run this script.")
elif None not in (x0, y0, x9, y9):
    # FIXED: az=+90 -> Y (left) high, X (front) ~centre;  az=0 -> X (front) high, Y ~centre
    left_at_90 = (y9 - 0.5) > 0.15 and abs(x9 - 0.5) < 0.15
    front_at_0 = (x0 - 0.5) > 0.15 and abs(y0 - 0.5) < 0.15
    front_at_90 = (x9 - 0.5) > 0.15 and abs(y9 - 0.5) < 0.15
    if left_at_90 and front_at_0:
        print("  VERDICT: FIXED  (+X=front, +Y=left).")
        print("    az=+90 -> Y(left)=%.3f high, X(front)=%.3f centre;  az=0 -> X(front)=%.3f high."
              % (y9, x9, x0))
        print("    Sweeping az 0 -> +90 moves the source FRONT -> LEFT. Correct.")
    elif front_at_90:
        print("  VERDICT: BUGGY frame  (+az drives the FRONT axis; front/side swapped).")
        print("    az=+90 -> X(front)=%.3f high, Y(left)=%.3f centre  (should be the other way)." % (x9, y9))
        print("    -> apply SET_DISTANCE_FIX.patch, rebuild + cmake --install, re-run this script.")
    else:
        print("  VERDICT: INCONCLUSIVE - source moves but not along the expected axes.")
        print("    az=0  X=%.3f Y=%.3f ;  az=+90  X=%.3f Y=%.3f" % (x0, y0, x9, y9))
else:
    print("  VERDICT: INCONCLUSIVE - missing X/Y readings.")
print("=" * 70)

if DO_CLEANUP:
    call("track.remove", track=track)
    print("\nscratch track removed (--cleanup).")
else:
    print("\nscene track %d LEFT IN PLACE - open RoomEncoder on it and watch the source dot "
          "(front wall = +X, left = +Y). Pass --cleanup to remove." % track)
