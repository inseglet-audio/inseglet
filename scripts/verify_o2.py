#!/usr/bin/env python3
"""Live-verify Batch O2 — per-object ADM metadata depth (extent / objectDivergence / importance).

O2 adds no tools; it enriches the O1 flagship. This gate proves, end-to-end on the real SDK path, that
`spatial.export_adm`'s `objectMetadata[]` authors extent / objectDivergence / importance into a real
ITU-R BS.2076 ADM BWF and that `analysis.adm_inspect` reads the footprint back — plus the backward-compat
guarantee that a metadata-free export carries NONE of it. The deterministic serialization + parser
round-trip live in `unit.adm`; this exercises the tool wiring on genuine REAPER renders.

Exercises:
  * spatial.export_adm  — objectMetadata per object: a `size`/divergence/importance object and a
                          width/height/depth object; echoes objectMetadataCount + per-object metadata.
  * analysis.adm_inspect — hasExtent / hasObjectDivergence / hasImportance + objectImportance values on
                          the metadata-rich file; all three false on the metadata-free file.

Tone (a stock JSFX) + an object panner (IEM StereoEncoder via spatial.ambisonic_encode) are BEST-EFFORT.
It builds its own scratch tracks, bounds every render, and writes to temp paths it deletes with --cleanup.
Nothing opens a dialog. REAPER must be FOREGROUND, and `cmake --install build` must have shipped the dylib.

Usage:
    python3 verify_o2.py                 # auto-find reaper_mcp.json; leaves scratch tracks + temp files
    python3 verify_o2.py --cleanup       # also delete scratch tracks + the temp ADM files
    python3 verify_o2.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 157  # O2 adds no tools — it enriches export_adm + adm_inspect
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]
ADM_META = "/tmp/mcp_o2_adm_meta.wav"
ADM_PLAIN = "/tmp/mcp_o2_adm_plain.wav"

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
    with urllib.request.urlopen(req, timeout=180) as r:
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
                              "clientInfo": {"name": "verify_o2", "version": "0"}})

passed = failed = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def add_tone(track):
    for nm in TONE_CANDIDATES:
        r = raw("fx.add", track=track, name=nm)
        if not r.get("isError") and r.get("structuredContent", {}).get("fxIndex", -1) >= 0:
            return True
    return False

def add_track(name):
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

# LIVE-GATE FINDING (M2): RENDER_BOUNDSFLAG 0 = custom range. Bound every render deterministically.
BOUNDS = dict(boundsFlag=0, startPos=0.0, endPos=0.5)

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (O2 adds no tools)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))

base = call("project.get_summary").get("trackCount", 0)

# scratch: a 7.1.2 bed (10 ch) + two mono objects. objA gets an IEM panner (readable position).
bed = add_track("MCP O2 bed 7.1.2")
call("spatial.set_track_channels", track=bed, channels=10)
toneBed = add_tone(bed)
objA = add_track("MCP O2 obj A")
toneA = add_tone(objA)
panOk = not failed_closed(raw("spatial.ambisonic_encode", track=objA, azimuthDeg=30.0,
                              elevationDeg=0.0, order=1))
objB = add_track("MCP O2 obj B")
add_tone(objB)
if not (toneBed and toneA):
    notes.append("no stock tone JSFX — audio is silence; metadata asserts still exercised.")
if not panOk:
    notes.append("spatial.ambisonic_encode unavailable — objA placed front-center (metadata unaffected).")

# ================================================================================================
print("\n== 1. export_adm — author per-object extent / divergence / importance ==")
meta = [
    {"track": objA, "size": 0.3, "divergence": 0.5, "divergenceRange": 30.0, "importance": 8},
    {"track": objB, "width": 20.0, "height": 10.0, "depth": 0.2, "importance": 3},
]
dry = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           objectMetadata=meta, dryRun=True, **BOUNDS)
check(dry.get("objectMetadataCount") == 2, "dryRun echoes objectMetadataCount = 2",
      "count=%s" % dry.get("objectMetadataCount"))
om = {o.get("track"): o.get("metadata") for o in dry.get("objects", []) if isinstance(o, dict)}
mA = om.get(objA) or {}
check(mA.get("importance") == 8 and abs((mA.get("divergence") or 0) - 0.5) < 1e-6,
      "objA metadata echoed: importance 8 + divergence 0.5",
      "meta=%s" % mA)
mB = om.get(objB) or {}
check(mB.get("importance") == 3 and (mB.get("width") or 0) == 20.0,
      "objB metadata echoed: importance 3 + width 20", "meta=%s" % mB)

exp = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           objectMetadata=meta, outPath=ADM_META, **BOUNDS)
check(exp.get("ok") is True and os.path.exists(ADM_META), "metadata-rich ADM written",
      "path=%s" % exp.get("path"))

# metadata-free export for the backward-compat proof
plain = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
             outPath=ADM_PLAIN, **BOUNDS)
check(plain.get("ok") is True and plain.get("objectMetadataCount") == 0,
      "metadata-free ADM written (objectMetadataCount 0)")

# ================================================================================================
print("\n== 2. adm_inspect — read the metadata footprint back ==")
ins = call("analysis.adm_inspect", path=ADM_META)
admj = ins.get("adm", {})
check(admj.get("hasExtent") is True, "metadata file: hasExtent true",
      "extentBlocks=%s" % admj.get("extentBlocks"))
check(admj.get("hasObjectDivergence") is True, "metadata file: hasObjectDivergence true",
      "divergenceBlocks=%s" % admj.get("divergenceBlocks"))
check(admj.get("hasImportance") is True, "metadata file: hasImportance true")
imps = sorted(admj.get("objectImportance", []))
check(imps == [3, 8], "objectImportance == [3, 8]", "values=%s" % admj.get("objectImportance"))

insp = call("analysis.adm_inspect", path=ADM_PLAIN)
admp = insp.get("adm", {})
check(admp.get("hasExtent") is False and admp.get("hasObjectDivergence") is False and
      admp.get("hasImportance") is False,
      "metadata-free file: extent/divergence/importance all absent (backward-compat)")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for f in (ADM_META, ADM_PLAIN):
        try:
            os.remove(f)
        except OSError:
            pass
    for t in sorted({t for t in (bed, objA, objB) if isinstance(t, int)}, reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base,
          "all scratch tracks removed", "count=%s base=%s"
          % (call("project.get_summary").get("trackCount"), base))
else:
    notes.append("scratch tracks + %s / %s left in place (pass --cleanup)." % (ADM_META, ADM_PLAIN))

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
