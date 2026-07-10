#!/usr/bin/env python3
"""Live-verify Batch M3 — metering depth (object loudness / binaural check / decode coverage).

Exercises, end-to-end on the real SDK path:
  * analysis.object_loudness — per-OBJECT BS.1770-4 gated loudness + energy share + dBTP in ONE
                               multi-stem pass, plus each object's panner position (best-effort) and
                               an optional bed-vs-objects level reference.
  * analysis.binaural_check  — 2-ch binaural/headphone-deliverable QC: program loudness, L/R
                               correlation, inter-aural level balance, mid/side ratio.
  * analysis.decode_coverage — ambisonic decode-coverage map: uniformity / CV / dead-zones /
                               front-back·left-right·up-down balance / dominant direction / hotspots,
                               on a uniform sphere AND a named delivery layout (7.1.4).

The DSP itself (gated loudness, the SN3D virtual-speaker decode, coverage statistics) is proven
DETERMINISTICALLY off-REAPER in `unit.meter`. This gate proves the TOOLS run end-to-end on real
REAPER renders and that the read-only guards hold: it builds its own scratch tracks, runs everything
bounded, and checks shapes + guards. Tone (a stock JSFX) and an ambisonic encode are BEST-EFFORT:
without them the plumbing still runs and level/direction asserts are noted as skipped. Nothing opens a
dialog. REAPER must be FOREGROUND, and `cmake --install build` must have shipped the current dylib.

Usage:
    python3 verify_m3.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_m3.py --cleanup       # also delete the scratch tracks it created
    python3 verify_m3.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 154  # 151 (Batch M2) + 3 (Batch M3)
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
    """Structured-error code of a RETURNED makeError (read-only analysis idiom: MCP isError stays
    false — the dispatcher only sets isError on a THROW/timeout), else None."""
    sc = result.get("structuredContent", {})
    return sc.get("error") if isinstance(sc, dict) else None

def failed_closed(result):
    return result.get("isError") is True or err_code(result) is not None

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_m3", "version": "0"}})

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
        if not r.get("isError"):
            sc = r.get("structuredContent", {})
            if sc.get("fxIndex", -1) >= 0:
                return True
    return False

def add_track(name):
    """Append at the CURRENT end (track.add echoes the requested index — request the true count)."""
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

# LIVE-GATE FINDING (M2): RENDER_BOUNDSFLAG 1 = ENTIRE PROJECT (not time selection). Bound every
# render with a CUSTOM range (boundsFlag=0 + startPos/endPos) so the gate is deterministic.
BOUNDS = dict(boundsFlag=0, startPos=0.0, endPos=0.5)

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (151 + 3 M3)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
m3 = ["analysis.object_loudness", "analysis.binaural_check", "analysis.decode_coverage"]
check(all(n in listed for n in m3), "all three M3 tools present in tools/list",
      "missing=%s" % [n for n in m3 if n not in listed])

base = call("project.get_summary").get("trackCount", 0)

# scratch content: a toned object, a silent object, a 5.1 bed (with tone), and a FOA scene.
objA = add_track("MCP M3 obj A (tone)")
toneA = add_tone(objA)
posOk = not failed_closed(raw("spatial.ambisonic_encode", track=objA, azimuthDeg=45.0,
                              elevationDeg=0.0, order=1))
objB = add_track("MCP M3 obj B (silent)")
bed = add_track("MCP M3 bed 5.1")
call("spatial.set_track_channels", track=bed, channels=6)
toneBed = add_tone(bed)
if not (toneA and toneBed):
    notes.append("no stock tone JSFX — level asserts skipped where noted (plumbing still exercised).")
if not posOk:
    notes.append("spatial.ambisonic_encode unavailable — object position not asserted.")

# ================================================================================================
print("\n== 1. analysis.object_loudness — per-object gated loudness + position + bed context ==")
bad = raw("analysis.object_loudness")
check(bad.get("isError") is True, "missing objects arg -> isError (required-arg contract)")

dup = raw("analysis.object_loudness", objects=[objA, objA])
check(failed_closed(dup) and err_code(dup) == "duplicate_object",
      "duplicate object -> fails closed (duplicate_object)", "error=%s" % err_code(dup))

dry = call("analysis.object_loudness", objects=[objA, objB], dryRun=True)
check(dry.get("dryRun") is True and dry.get("count") == 2 and "plan" in dry,
      "object_loudness dryRun: plan + 2 objects, no render")

ol = call("analysis.object_loudness", objects=[objA, objB], bed=bed, **BOUNDS)
objs = ol.get("objects", [])
check(len(objs) == 2 and ol.get("count") == 2, "two per-object rows returned",
      "count=%s rows=%d" % (ol.get("count"), len(objs)))
fields_ok = all(all(k in o for k in ("lufsIntegrated", "ungatedLufs", "activityFraction",
                                     "truePeakDb", "energyShare", "deltaFromLoudestLu",
                                     "position")) for o in objs)
check(fields_ok, "every object row has gated/ungated/activity/truePeak/energyShare/delta/position")
check(isinstance(ol.get("bed"), dict), "bed context returned when bed= is passed")
if objs:
    shares = [o.get("energyShare", 0) for o in objs]
    check(all(0.0 <= s <= 1.0 for s in shares), "energy shares in [0,1]", "shares=%s" % shares)
byTrack = {o["track"]: o for o in objs}
a, b = byTrack.get(objA, {}), byTrack.get(objB, {})
if toneA:
    check(a.get("lufsIntegrated", -999) > -70.0, "toned object measures above the -70 gate",
          "lufs=%s" % a.get("lufsIntegrated"))
    check(a.get("energyShare", 0) > b.get("energyShare", 1),
          "toned object carries more energy share than the silent one",
          "toned=%s silent=%s" % (a.get("energyShare"), b.get("energyShare")))
if posOk:
    pos = a.get("position")
    check(isinstance(pos, dict) and any("azim" in k.lower() for k in pos.keys()),
          "toned object's panner position read (azimuth present)", "position=%s" % pos)

# ================================================================================================
print("\n== 2. analysis.binaural_check — 2-ch binaural/headphone QC ==")
notbin = raw("analysis.binaural_check", target=bed)
check(failed_closed(notbin) and err_code(notbin) == "not_binaural",
      "6-ch bed -> fails closed (not_binaural)", "error=%s" % err_code(notbin))

# Build a real binaural monitor bus off a scene when a decoder is available; else a 2-ch tone track.
scene = add_track("MCP M3 scene")
scene_tone = add_tone(scene)
scene_enc = not failed_closed(raw("spatial.ambisonic_encode", track=scene, azimuthDeg=90.0,
                                  elevationDeg=0.0, order=1))
if not scene_enc:
    call("spatial.set_track_channels", track=scene, channels=4)

binbus = None
mon = raw("spatial.add_binaural_monitor", track=scene) if scene_enc else {"isError": True}
if not mon.get("isError"):
    binbus = mon.get("structuredContent", {}).get("monitorTrack")
if binbus is None:
    binbus = add_track("MCP M3 binaural (stereo)")
    add_tone(binbus)
    notes.append("no binaural decoder — metering a plain stereo tone bus instead of a decode.")

dry = call("analysis.binaural_check", target=binbus, dryRun=True)
check(dry.get("dryRun") is True and "plan" in dry, "binaural_check dryRun: plan, no render")

bc = call("analysis.binaural_check", target=binbus, **BOUNDS)
check(all(k in bc for k in ("program", "left", "right", "correlation",
                            "interAuralBalanceDb", "midSide", "interpretation")),
      "binaural_check returns program/left/right/correlation/balance/midSide/interpretation")
corr = bc.get("correlation")
check(isinstance(corr, (int, float)) and -1.0 <= corr <= 1.0, "L/R correlation in [-1,1]",
      "corr=%s" % corr)
bal = bc.get("interAuralBalanceDb")
check(isinstance(bal, (int, float)), "inter-aural balance is a number", "balance=%s" % bal)
ms = bc.get("midSide", {})
check(all(k in ms for k in ("midRmsDb", "sideRmsDb", "sideToMidDb")),
      "midSide reports mid/side RMS + side/mid ratio")

# ================================================================================================
print("\n== 3. analysis.decode_coverage — ambisonic spatial coverage map ==")
stereo2 = add_track("MCP M3 stereo")  # a genuine 2-ch target for the guard (binbus is a wider bus)
notamb = raw("analysis.decode_coverage", target=stereo2)
check(failed_closed(notamb) and err_code(notamb) == "not_ambisonic",
      "2-ch target -> fails closed (not_ambisonic)", "error=%s" % err_code(notamb))

dry = call("analysis.decode_coverage", target=scene, speakers=96, dryRun=True)
check(dry.get("dryRun") is True and dry.get("mode") == "uniform-sphere" and
      dry.get("speakers") == 96 and "plan" in dry,
      "decode_coverage dryRun: uniform-sphere plan, 96 speakers")

dc = call("analysis.decode_coverage", target=scene, speakers=96, **BOUNDS)
check(all(k in dc for k in ("uniformity", "coefficientOfVariation", "deadZones", "balance",
                            "dominant", "hotspots", "interpretation")),
      "decode_coverage returns uniformity/CV/deadZones/balance/dominant/hotspots/interpretation")
bal = dc.get("balance", {})
check(all(k in bal for k in ("frontFrac", "backFrac", "leftFrac", "rightFrac",
                             "upperFrac", "lowerFrac", "earLevelFrac")),
      "balance reports front/back·left/right·upper/lower·ear-level fractions")
u = dc.get("uniformity")
check(isinstance(u, (int, float)) and 0.0 <= u <= 1.0, "uniformity in [0,1]", "uniformity=%s" % u)
check(isinstance(dc.get("hotspots"), list) and len(dc.get("hotspots", [])) >= 1,
      "hotspots returned (top energy directions)")
if scene_enc and scene_tone:
    dom = dc.get("dominant", {})
    az = dom.get("azimuthDeg", 0.0)
    # source encoded at ambisonic az=90 (left); dominant decode direction should land near it and the
    # left hemisphere should carry more energy than the right.
    d = abs(((az - 90.0) + 540.0) % 360.0 - 180.0)
    check(d < 35.0, "dominant decode direction near the encoded az 90", "az=%.1f" % az)
    check(bal.get("leftFrac", 0) > bal.get("rightFrac", 1),
          "left-encoded source: left hemisphere carries more energy",
          "L=%s R=%s" % (bal.get("leftFrac"), bal.get("rightFrac")))

# named-layout decode (7.1.4): 11 directional speakers, per-speaker energy returned.
lay = call("analysis.decode_coverage", target=scene, layout="7.1.4", **BOUNDS)
check(lay.get("mode") == "layout:7.1.4" and lay.get("speakers") == 11,
      "7.1.4 layout decode: 11 directional speakers", "mode=%s n=%s"
      % (lay.get("mode"), lay.get("speakers")))
ps = lay.get("perSpeaker")
check(isinstance(ps, list) and len(ps) == 11 and all("label" in s and "energyFrac" in s for s in ps),
      "per-speaker energy returned for the named layout (labelled)")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    scratch = [objA, objB, bed, scene, binbus, stereo2]
    for t in sorted({t for t in scratch if isinstance(t, int)}, reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base,
          "all scratch tracks removed", "count=%s base=%s"
          % (call("project.get_summary").get("trackCount"), base))
else:
    notes.append("scratch tracks left in place (pass --cleanup to remove).")

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
