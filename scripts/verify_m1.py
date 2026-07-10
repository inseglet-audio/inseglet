#!/usr/bin/env python3
"""Live-verify Batch M1 — immersive metering suite (2 tools) against a running REAPER + extension.

Exercises, end-to-end on the real SDK path (render -> read WAV -> DSP -> temp cleanup -> RENDER_* restore):
  * analysis.meter          — program loudness (RENDER_STATS) + per-channel level/peak/true-peak/K-level
                              + L/R correlation, on a 5.1 bed. Proves the render->read->metrics plumbing.
  * analysis.spatial_field  — DirAC DoA + diffuseness + Gerzon rV/rE on an ACN/SN3D FOA scene bus.

The measurement DSP itself (per-channel numbers, K-weighting, true-peak, DoA to 2 deg, rE/rV) is proven
DETERMINISTICALLY off-REAPER in `unit.meter`. This live gate proves the TOOLS run end-to-end on a real
REAPER render and that the read-only guards hold: it builds its own scratch tracks, runs both tools, and
checks the output shape + guards. Audio content (a stock tone JSFX) and a real ambisonic encode
(spatial.ambisonic_encode, needs a spatial suite like IEM) are BEST-EFFORT: if unavailable, the plumbing is
still exercised on a bounded silent render and the direction/level asserts are noted as skipped. Nothing
opens a dialog. REAPER must be in the FOREGROUND, and `cmake --install build` must have shipped the current
dylib. Stdlib only.

Usage:
    python3 verify_m1.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_m1.py --cleanup       # also delete the scratch tracks it created
    python3 verify_m1.py /path/to/reaper_mcp.json
"""
import json
import math
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 147  # 145 (Batch S1) + 2 (Batch M1)
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]

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

def call(tool_name, **arguments):
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError("%s isError: %s" % (tool_name, r.get("content")))
    return r.get("structuredContent", {})

def raw(tool_name, **arguments):
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})

def err_text(result):
    return " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_m1", "version": "0"}})

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
    """Best-effort insert of a stock tone/oscillator JSFX so the render has real audio. Returns True/False."""
    for nm in TONE_CANDIDATES:
        r = raw("fx.add", track=track, name=nm)
        if not r.get("isError"):
            sc = r.get("structuredContent", {})
            if sc.get("fxIndex", -1) >= 0:
                return True
    return False

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (145 + 2 M1)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
m1 = ["analysis.meter", "analysis.spatial_field"]
check(all(n in listed for n in m1), "both M1 tools present in tools/list",
      "missing=%s" % [n for n in m1 if n not in listed])

base = call("project.get_summary").get("trackCount", 0)

def add_track(name):
    """Append a track at the CURRENT end (track.add echoes the requested index, so request the true
    count — requesting past it would clamp to append but leave us pointing at a phantom index)."""
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

# ================================================================================================
print("\n== 1. analysis.meter — 5.1 bed: dryRun plan, per-channel, LFE flag, correlation ==")
bed = add_track("MCP M1 meter bed")
call("spatial.set_track_channels", track=bed, channels=6)   # 5.1
check(call("track.get", track=bed).get("channels") == 6, "bed track sized to 6 ch (5.1)")

dry = call("analysis.meter", target=bed, dryRun=True)
check(dry.get("dryRun") is True and dry.get("layout") == "5.1" and "plan" in dry,
      "meter dryRun returns a plan + layout '5.1' (no render)", "layout=%s" % dry.get("layout"))

tone_ok = add_tone(bed)
if not tone_ok:
    notes.append("no stock tone JSFX loaded — meter runs on a bounded SILENT render (plumbing only).")
call("timeselection.set", start=0.0, end=0.5)               # bound the render so it fits the call window

m = call("analysis.meter", target=bed, boundsFlag=1)
cd = m.get("channelsDetail", [])
check(m.get("channels") == 6 and len(cd) == 6, "meter returns 6 per-channel detail rows",
      "channels=%s rows=%d" % (m.get("channels"), len(cd)))
check(m.get("layout") == "5.1", "meter reports layout 5.1")
if len(cd) == 6:
    labels = [c.get("label") for c in cd]
    check(labels[2] == "C" and labels[3] == "LFE", "SMPTE labels: idx2=C, idx3=LFE",
          "labels=%s" % labels)
    check(cd[3].get("isLFE") is True and cd[0].get("isLFE") is False,
          "LFE flagged at index 3 only")
    fields_ok = all(all(k in c for k in ("rmsDb", "peakDb", "truePeakDb", "kLevelLkfs")) for c in cd)
    check(fields_ok, "every channel row has rms/peak/truePeak/kLevel")
check(isinstance(m.get("downmix"), dict) and "lrCorrelation" in m.get("downmix", {}),
      "meter reports L/R downmix correlation")
if tone_ok and len(cd) == 6:
    peaks = [c.get("peakDb", -150) for c in cd]
    check(max(peaks) > -80.0, "at least one channel has real signal (peak > -80 dBFS)",
          "maxPeak=%.1f" % max(peaks))
    tp = max(c.get("truePeakDb", -150) for c in cd)
    check(tp > -150.0 and all(math.isfinite(c.get("truePeakDb", 0.0)) for c in cd),
          "per-channel true-peak (dBTP) is finite", "maxTP=%.1f" % tp)

# ================================================================================================
print("\n== 2. analysis.spatial_field — FOA scene: guard, dryRun, live field ==")
def err_code(result):
    """The structured-error code of a RETURNED makeError result (read-only analysis idiom sets no MCP
    isError — the dispatcher only flags isError on a THROW/timeout), else None."""
    sc = result.get("structuredContent", {})
    return sc.get("error") if isinstance(sc, dict) else None

def failed_closed(result):
    """True if the tool refused — either a thrown isError OR a returned structured error object."""
    return result.get("isError") is True or err_code(result) is not None

# (a) fail-closed guard on a non-ambisonic (stereo) width
stereo = add_track("MCP M1 stereo")
g = raw("analysis.spatial_field", target=stereo)
gtxt = (err_text(g) + " " + json.dumps(g.get("structuredContent", {}))).lower()
check(failed_closed(g) and (err_code(g) == "not_ambisonic" or "ambisonic" in gtxt),
      "spatial_field on a stereo track -> fails closed (not_ambisonic)",
      "isError=%s error=%s" % (g.get("isError"), err_code(g)))

# (b) build a real FOA scene: tone -> ambisonic encoder at az=90 (best-effort; needs a spatial suite)
scene = add_track("MCP M1 scene")
scene_tone = add_tone(scene)
encoded = False
enc = raw("spatial.ambisonic_encode", track=scene, azimuthDeg=90.0, elevationDeg=0.0, order=1)
if not failed_closed(enc):
    encoded = True
else:
    # no encoder suite — still size the bus so the plumbing path runs on a (silent/uncorrelated) FOA width
    call("spatial.set_track_channels", track=scene, channels=4)
    notes.append("spatial.ambisonic_encode unavailable (%s) — DoA not asserted; field plumbing only."
                 % err_text(enc)[:60])
check(call("track.get", track=scene).get("channels") == 4, "scene bus is 4-ch FOA width")

dry2 = call("analysis.spatial_field", target=scene, dryRun=True)
check(dry2.get("dryRun") is True and dry2.get("order") == 1 and "plan" in dry2,
      "spatial_field dryRun returns a plan + order 1 (no render)")

sf = call("analysis.spatial_field", target=scene, boundsFlag=1)
check(sf.get("order") == 1 and sf.get("acnUsed") == 4, "spatial_field: order 1, 4 ACN used",
      "order=%s acnUsed=%s" % (sf.get("order"), sf.get("acnUsed")))
diff = sf.get("diffuseness")
check(isinstance(diff, (int, float)) and 0.0 <= diff <= 1.0, "diffuseness in [0,1]", "psi=%s" % diff)
rv = sf.get("velocityVector", {})
re = sf.get("energyVector", {})
check("magnitude" in rv and "azimuthDeg" in rv and "magnitude" in re and "azimuthDeg" in re,
      "spatial_field returns rV + rE (magnitude + direction)")
if encoded and scene_tone:
    doa = sf.get("doa", {})
    az = doa.get("azimuthDeg")
    # a real focused encode should read a low-ish diffuseness and a DoA near the encoded 90 deg.
    if isinstance(diff, (int, float)) and diff < 0.6 and isinstance(az, (int, float)):
        d = abs(((az - 90.0) + 540.0) % 360.0 - 180.0)
        check(d < 35.0, "live DoA azimuth near the encoded 90 deg (focused field)",
              "az=%.1f diffuseness=%.2f" % (az, diff))
    else:
        notes.append("encoded field not focused (diffuseness=%s) — live DoA not asserted; "
                     "exact DoA is proven in unit.meter." % diff)
elif not scene_tone:
    notes.append("no tone on the scene bus — spatial_field ran on silence; direction not asserted.")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    # remove highest index first so earlier indices stay valid
    for t in sorted([bed, stereo, scene], reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base, "all scratch tracks removed")
else:
    notes.append("scratch tracks %s left in place (pass --cleanup to remove)" % sorted([bed, stereo, scene]))

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
