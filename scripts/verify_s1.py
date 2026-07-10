#!/usr/bin/env python3
"""Live-verify Batch S1 — spatial / ambisonic DEPTH (6 tools) against a running REAPER + extension.

Exercises, end-to-end on the real SDK path:
  * spatial.convert_order  — HOA order change by ACN channel-count resize (down=truncate, up=zero-pad)
  * spatial.convert_format — normalization / format conversion via the bundled JSFX 'MCP Ambisonic
                             Format Converter' (SN3D<->N3D any order; FuMa FOA-only; no-op; refusals)
  * spatial.get_scene_info — read-only order inference + ACN padding report
  * spatial.mirror_scene   — reflection via the bundled JSFX 'MCP Ambisonic Scene Mirror'
  * spatial.beamform       — SPARTA Beamformer orchestration (fail-closed if absent)
  * spatial.set_distance   — IEM RoomEncoder orchestration (fail-closed if absent)

The gate builds ONE scratch track, resizes it across ambisonic orders, inserts the two bundled JSFX and
round-trips their mode/plane params (proving the JSFX shipped by 'cmake --install' loaded AND the tool
wrote the param), and probes the two plug-in tools in a fail-closed-or-drive way (a clean install hint
when the suite is absent is a PASS; a real insert when present is also a PASS). Nothing opens a dialog.
REAPER must be in the FOREGROUND, and 'cmake --install build' must have shipped the JSFX to Effects/MCP/.
Stdlib only.

Usage:
    python3 verify_s1.py                 # auto-find reaper_mcp.json; leaves the scratch track
    python3 verify_s1.py --cleanup       # also delete the scratch track it created
    python3 verify_s1.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
MIN_SURFACE = 145  # 139 (post-patch_state) + 6 (Batch S1) floor; the surface only grows with later
                   # batches, so verify the floor is met, not an exact count (was == 145, now stale).

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

def err_text(result):
    """Concatenate the text content of an isError result."""
    return " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_s1", "version": "0"}})

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
print("== 0. surface >= %d ==" % MIN_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") >= MIN_SURFACE,
      "tool surface >= %d (139 + 6 S1 floor; grows with later batches)" % MIN_SURFACE,
      "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
s1 = ["spatial.convert_order", "spatial.convert_format", "spatial.get_scene_info",
      "spatial.mirror_scene", "spatial.beamform", "spatial.set_distance"]
check(all(n in listed for n in s1), "all 6 S1 tools present in tools/list",
      "missing=%s" % [n for n in s1 if n not in listed])

# --- scratch setup ---
base = call("project.get_summary").get("trackCount", 0)
track = call("track.add", index=base, name="MCP S1 scratch").get("trackIndex", base)
print("\nscratch track: %d\n" % track)

print("== 1. convert_order: up to 3rd order -> 16-ch ACN bus ==")
o3 = call("spatial.convert_order", track=track, toOrder=3)
check(o3.get("hoaChannels") == 16 and o3.get("channels") == 16 and o3.get("padded") is False,
      "convert_order toOrder=3 -> 16 ACN ch, unpadded", "channels=%s" % o3.get("channels"))
check(call("track.get", track=track).get("channels") == 16,
      "track.get confirms 16 channels")
si = call("spatial.get_scene_info", track=track)
check(si.get("inferredOrder") == 3 and si.get("hoaChannels") == 16 and si.get("padded") is False,
      "get_scene_info infers 3rd order from 16 ch", "inferred=%s" % si.get("inferredOrder"))

print("\n== 2. convert_order: DOWN to 1st (truncate) then UP to 2nd (zero-pad) ==")
d1 = call("spatial.convert_order", track=track, toOrder=1)
check(d1.get("channels") == 4 and "down" in (d1.get("direction") or ""),
      "convert_order 3->1 truncates to 4 ch (down)", "dir=%s" % d1.get("direction"))
check(call("track.get", track=track).get("channels") == 4, "track.get confirms 4 channels after truncate")
u2 = call("spatial.convert_order", track=track, toOrder=2)
check(u2.get("channels") == 10 and u2.get("hoaChannels") == 9 and u2.get("padded") is True
      and "up" in (u2.get("direction") or ""),
      "convert_order 1->2 zero-pads to a 10-ch bus (9 ACN + 1 pad)", "dir=%s" % u2.get("direction"))
check(any("zero-pad" in w for w in u2.get("warnings", [])),
      "up-order emits the honest zero-pad warning")

print("\n== 3. get_scene_info: read-only order/padding report on the 2nd-order bus ==")
si2 = call("spatial.get_scene_info", track=track)
check(si2.get("inferredOrder") == 2 and si2.get("hoaChannels") == 9 and si2.get("padded") is True
      and si2.get("surplusChannels") == 1,
      "get_scene_info: order2, 9 ACN, 1 surplus (padded)", "surplus=%s" % si2.get("surplusChannels"))
check(isinstance(si2.get("spatialFx"), list), "get_scene_info returns a spatialFx[] list")

print("\n== 4. convert_format: JSFX insert + mode round-trip + no-op + refusals ==")
fx_before = call("fx.list", track=track).get("fxCount", 0)
cf = call("spatial.convert_format", track=track, **{"from": "N3D", "to": "SN3D"})
check(cf.get("fxIndex", -1) >= 0 and "Format Converter" in (cf.get("converter") or "")
      and cf.get("mode") == 1,
      "convert_format N3D->SN3D inserts the bundled converter JSFX (mode 1)",
      "converter=%r fx=%s" % (cf.get("converter"), cf.get("fxIndex")))
check(call("fx.list", track=track).get("fxCount", 0) == fx_before + 1,
      "fx count increased by 1 (JSFX landed)")
mp = cf.get("modeParam")
if isinstance(mp, int) and mp >= 0:
    pv = call("fx.get_param", track=track, fx=cf["fxIndex"], param=mp).get("value")
    check(pv is not None and abs(pv - 1.0) < 0.5,
          "JSFX 'Conversion' param round-trips to mode 1 (JSFX compiled + tool wrote it)", "value=%s" % pv)
else:
    check(False, "convert_format returned a usable modeParam", "modeParam=%s" % mp)
noop = call("spatial.convert_format", track=track, **{"from": "SN3D", "to": "ambiX"})
check(noop.get("noop") is True and noop.get("fxIndex") == -1,
      "convert_format SN3D->ambiX is a no-op (no FX added)")
uns = raw("spatial.convert_format", **{"track": track, "from": "N3D", "to": "FuMa"})
check(uns.get("isError") is True, "convert_format N3D->FuMa (unsupported direct) -> isError")
# FuMa on this 10-ch HOA bus must be refused (first-order only)
fumaHOA = raw("spatial.convert_format", **{"track": track, "from": "SN3D", "to": "FuMa"})
check(fumaHOA.get("isError") is True and "first-order" in err_text(fumaHOA).lower(),
      "convert_format SN3D->FuMa on a 10-ch HOA bus -> refused (first-order only)")

print("\n== 5. FuMa is accepted at first order (B-format) ==")
call("spatial.convert_order", track=track, toOrder=1)  # 4-ch FOA carrier
fo = call("spatial.convert_format", track=track, **{"from": "SN3D", "to": "FuMa"})
check(fo.get("fxIndex", -1) >= 0 and fo.get("mode") == 2,
      "convert_format SN3D->FuMa on a 4-ch FOA bus -> mode 2 (accepted)", "mode=%s" % fo.get("mode"))

print("\n== 6. mirror_scene: JSFX insert + plane->index + param round-trip ==")
mr = call("spatial.mirror_scene", track=track, plane="front-back")
check(mr.get("fxIndex", -1) >= 0 and "Scene Mirror" in (mr.get("mirror") or "")
      and mr.get("planeIndex") == 1,
      "mirror_scene front-back inserts the bundled mirror JSFX (planeIndex 1)",
      "mirror=%r" % mr.get("mirror"))
pp = mr.get("planeParam")
if isinstance(pp, int) and pp >= 0:
    pv = call("fx.get_param", track=track, fx=mr["fxIndex"], param=pp).get("value")
    check(pv is not None and abs(pv - 1.0) < 0.5,
          "JSFX 'Reflection plane' param round-trips to 1 (front-back)", "value=%s" % pv)
else:
    check(False, "mirror_scene returned a usable planeParam", "planeParam=%s" % pp)
mr0 = call("spatial.mirror_scene", track=track, plane="left-right")
check(mr0.get("planeIndex") == 0, "mirror_scene left-right -> planeIndex 0")

print("\n== 7. beamform: SPARTA Beamformer (fail-closed if absent, drive if present) ==")
bf = raw("spatial.beamform", track=track, azimuthDeg=45.0, elevationDeg=10.0)
if bf.get("isError"):
    check("beamformer" in err_text(bf).lower(),
          "beamform fails closed with a beamformer install hint (SPARTA absent)")
    notes.append("SPARTA Beamformer not installed — beamform verified fail-closed only.")
else:
    sc = bf.get("structuredContent", {})
    check(sc.get("ok") is True and sc.get("beamformer"),
          "beamform drove a live beamformer", "beamformer=%r" % sc.get("beamformer"))

print("\n== 8. set_distance: IEM RoomEncoder (fail-closed if absent, drive if present) ==")
rm = raw("spatial.set_distance", track=track, distanceM=3.0, azimuthDeg=30.0, reflectionOrder=2)
if rm.get("isError"):
    check("room encoder" in err_text(rm).lower() or "roomencoder" in err_text(rm).lower(),
          "set_distance fails closed with a RoomEncoder install hint (IEM absent)")
    notes.append("IEM RoomEncoder not installed — set_distance verified fail-closed only.")
else:
    sc = rm.get("structuredContent", {})
    check(sc.get("ok") is True and sc.get("roomEncoder"),
          "set_distance drove a live RoomEncoder", "roomEncoder=%r" % sc.get("roomEncoder"))

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
