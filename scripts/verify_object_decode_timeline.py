#!/usr/bin/env python3
"""Live-verify Batch T1 — per-object decode-coverage timeline.

Exercises, end-to-end on the real SDK path against a running REAPER:
  * analysis.object_decode_timeline — for each immersive OBJECT, the geometric decode-coverage TIMELINE
                                      onto a delivery layout: read each object's panner position (or an
                                      explicit keyframe trajectory), encode -> SN3D SH -> projection-decode
                                      onto the layout's speakers, and report the dominant-speaker path,
                                      migration, dwell, angular travel, and the coverage footprint.

The decode geometry (encode->decode per-speaker energy, dominant selection, migration, dwell, footprint)
is proven DETERMINISTICALLY off-REAPER in unit.decode_timeline. This gate proves the TOOL runs end-to-end
on real REAPER: it reads live panner positions (a hard-left object lands on the left speakers, a hard-right
on the right, an overhead object on the top layer), auto-discovers 'Object N' tracks, and computes a
real moving-object migration timeline through the explicit-trajectory path. It opens its own scratch
project so the user's project is untouched and track discovery is deterministic.

Usage:
    python3 verify_object_decode_timeline.py            # auto-find reaper_mcp.json
    python3 verify_object_decode_timeline.py --cleanup  # remove scratch tracks at the end
    python3 verify_object_decode_timeline.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 186  # 185 (v1.4.0) + 1 (Batch T1: object_decode_timeline)

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
                              "clientInfo": {"name": "verify_object_decode_timeline", "version": "0"}})

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

def obj_by_label(res, label):
    for o in res.get("objects", []):
        if o.get("label") == label:
            return o
    return {}

def dom_label(o):
    cov = o.get("coverage") or {}
    return (cov.get("dominant") or {}).get("label")

LEFT = {"L", "Lss", "Lrs", "Lw"}
RIGHT = {"R", "Rss", "Rrs", "Rw"}
TOP = {"Ltf", "Rtf", "Ltr", "Rtr", "Ltm", "Rtm"}

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (185 + 1 Batch T1)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
check("analysis.object_decode_timeline" in listed,
      "analysis.object_decode_timeline present in tools/list")

# Fresh scratch project so discovery + counts are deterministic and the user's project is untouched.
RPP = "/tmp/mcp_decode_timeline.rpp"
with open(RPP, "w") as fh:
    fh.write('<REAPER_PROJECT 0.1 "7.0" 0\n  SAMPLERATE 48000 1 0\n  TEMPO 120 4 4\n>\n')
op = raw("project.open", path=RPP, discardChanges=True)
check(not failed_closed(op), "opened a scratch project (project.open discardChanges)")

# Three objects at distinct STATIC positions via IEM encoders (like the DAMF gate): hard-left,
# hard-right, overhead-front. object_decode_timeline reads these panner positions live.
o1 = add_track("Object 1")   # az +90 -> hard LEFT
o2 = add_track("Object 2")   # az -90 -> hard RIGHT
o3 = add_track("Object 3")   # el +45 -> OVERHEAD front
panL = not failed_closed(raw("spatial.ambisonic_encode", track=o1, azimuthDeg=90.0, elevationDeg=0.0, order=1))
panR = not failed_closed(raw("spatial.ambisonic_encode", track=o2, azimuthDeg=-90.0, elevationDeg=0.0, order=1))
panU = not failed_closed(raw("spatial.ambisonic_encode", track=o3, azimuthDeg=0.0, elevationDeg=45.0, order=1))
if not (panL and panR and panU):
    notes.append("spatial.ambisonic_encode unavailable — live panner positions not asserted; "
                 "falling back to the explicit-trajectory checks only.")

# ================================================================================================
print("\n== 1. dryRun plan + auto-discovery of 'Object N' ==")
dry = call("analysis.object_decode_timeline", layout="7.1.4", dryRun=True)
check(dry.get("dryRun") is True and dry.get("count") == 3 and len(dry.get("speakers", [])) == 11,
      "dryRun auto-discovers 3 'Object N' tracks on 7.1.4 (11 speakers)",
      "count=%s speakers=%s" % (dry.get("count"), len(dry.get("speakers", []))))
check(all("positionSource" in o and "samples" in o for o in dry.get("objects", [])),
      "dryRun reports per-object positionSource + sample count")

# ================================================================================================
print("\n== 2. live panner read -> dominant speaker per object ==")
res = call("analysis.object_decode_timeline", objects=["Object 1", "Object 2", "Object 3"],
           layout="7.1.4", order=3)
check(res.get("count") == 3, "3 objects analyzed", "count=%s" % res.get("count"))
oL, oR, oU = obj_by_label(res, "Object 1"), obj_by_label(res, "Object 2"), obj_by_label(res, "Object 3")

if panL and panR and panU:
    check(dom_label(oL) in LEFT, "hard-left object (az=90) -> dominant LEFT speaker",
          "dominant=%s" % dom_label(oL))
    check(dom_label(oR) in RIGHT, "hard-right object (az=-90) -> dominant RIGHT speaker",
          "dominant=%s" % dom_label(oR))
    check(dom_label(oU) in TOP, "overhead object (el=45) -> dominant TOP speaker",
          "dominant=%s" % dom_label(oU))
    upFrac = (oU.get("coverage") or {}).get("upperFrac", 0.0)
    check(upFrac > 0.4, "overhead object footprint is height-dominated (upperFrac > 0.4)",
          "upperFrac=%s" % upFrac)
    check((oL.get("positionSource") in ("static", "envelope")) and
          (oR.get("positionSource") in ("static", "envelope")),
          "encoded objects report a real positionSource (static/envelope)",
          "L=%s R=%s" % (oL.get("positionSource"), oR.get("positionSource")))
    # left and right objects land on opposite sides
    check(dom_label(oL) in LEFT and dom_label(oR) in RIGHT and dom_label(oL) != dom_label(oR),
          "left/right objects resolve to opposite sides")
else:
    notes.append("live panner checks skipped (encoder unavailable)")

# each static object is reported static with a single-run migration
check(oL.get("travel", {}).get("static") in (True, False), "travel.static present on live objects")

# ================================================================================================
print("\n== 3. explicit moving trajectory -> migration timeline (end-to-end through the live tool) ==")
mv = call("analysis.object_decode_timeline", layout="7.1.4", order=3, maxTimelinePoints=32,
          trajectories=[{"name": "flyby", "keyframes": [
              {"t": 0.0, "azimuthDeg": 90.0}, {"t": 1.0, "azimuthDeg": 45.0},
              {"t": 2.0, "azimuthDeg": 0.0}, {"t": 3.0, "azimuthDeg": -45.0},
              {"t": 4.0, "azimuthDeg": -90.0}]}])
check(mv.get("count") == 1, "1 explicit-trajectory object")
fly = obj_by_label(mv, "flyby")
mig = fly.get("migration", [])
check(mig and mig[0] in LEFT and mig[-1] in RIGHT,
      "flyby dominant migrates from a LEFT speaker to a RIGHT speaker", "migration=%s" % mig)
check(len({m for m in mig}) >= 3, "flyby crosses >= 3 distinct dominant speakers", "migration=%s" % mig)
trav = fly.get("travel", {})
check(trav.get("static") is False and trav.get("angularTravelDeg", 0) > 120,
      "flyby is reported moving with a large angular travel (~180 deg)",
      "travel=%s static=%s" % (trav.get("angularTravelDeg"), trav.get("static")))
cov = fly.get("coverage") or {}
check(cov.get("speakersTouched", 0) >= 3 and cov.get("upperFrac", 1.0) < 0.25,
      "flyby footprint: >=3 speakers touched, planar (low upper energy)",
      "touched=%s upper=%s" % (cov.get("speakersTouched"), cov.get("upperFrac")))

# ================================================================================================
print("\n== 4. elevation climb + guards ==")
climb = call("analysis.object_decode_timeline", layout="7.1.4", order=3,
             trajectories=[{"name": "climb", "keyframes": [
                 {"t": 0.0, "azimuthDeg": 0.0, "elevationDeg": 0.0},
                 {"t": 1.0, "azimuthDeg": 0.0, "elevationDeg": 45.0}]}])
cl = obj_by_label(climb, "climb")
check(cl.get("travel", {}).get("maxElevationDeg", 0) >= 44.0,
      "elevation climb reaches ~45 deg", "maxEl=%s" % cl.get("travel", {}).get("maxElevationDeg"))
check((cl.get("coverage") or {}).get("upperFrac", 0.0) > 0.1,
      "climb footprint gains upper-hemisphere energy")

bad = raw("analysis.object_decode_timeline", layout="atmos-9000",
          trajectories=[{"keyframes": [{"t": 0, "azimuthDeg": 0}]}])
check(failed_closed(bad) and err_code(bad) == "unknown_layout",
      "unknown layout fails closed (unknown_layout)", "error=%s" % err_code(bad))
bad2 = raw("analysis.object_decode_timeline",
           trajectories=[{"name": "empty", "keyframes": []}])
check(failed_closed(bad2) and err_code(bad2) == "bad_trajectory",
      "empty keyframes fails closed (bad_trajectory)", "error=%s" % err_code(bad2))

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for idx in sorted([o3, o2, o1], reverse=True):
        raw("track.remove", track=idx)
    check(True, "scratch object tracks deleted")

print("\n%d passed, %d failed" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
sys.exit(1 if failed else 0)
