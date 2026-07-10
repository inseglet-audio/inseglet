#!/usr/bin/env python3
"""Live-verify Batch O3 — Dolby Atmos Master ADM Profile v1.0 conformance.

O3 adds one tool (`analysis.adm_profile_check`, surface 157 -> 158) and a `profile:"dolby-atmos"` mode on
`spatial.export_adm`. This gate proves, end-to-end on the real SDK path, that the profile mode authors a
CONFORMANT ADM BWF from a model that carried non-conformant O2 metadata (divergence + importance + unequal
extent) — the normalizer strips/collapses it, forces cartesian, and the written file passes
`analysis.adm_profile_check`; while the same material exported WITHOUT the profile is flagged
non-conformant with the expected violation codes. The deterministic normalize/validate math lives in
`unit.adm_profile`; this exercises the tool wiring on genuine REAPER renders.

Exercises:
  * spatial.export_adm profile:"dolby-atmos" — dryRun echoes a conformant profile block + normalized[] +
    cartesian; a real write produces a file that adm_profile_check passes and adm_inspect shows STRIPPED
    (no divergence, no importance, cartesian) — proof the normalization reached the bytes.
  * analysis.adm_profile_check — conformant on the profile file; non-conformant (with divergence /
    importance / coordinate / extent / interpolation codes) on a raw O2 export; file_not_found fail-closed.
  * fail-closed: profile on a non-Atmos 7.1.4 bed -> bed_not_profile_conformant.

Tone (a stock JSFX) + an object panner (IEM StereoEncoder) are BEST-EFFORT. It builds its own scratch
tracks, bounds every render, writes to temp paths it deletes with --cleanup. Nothing opens a dialog.
REAPER must be FOREGROUND, and `cmake --install build` must have shipped the dylib. The profile mandates a
48 kHz deliverable: if the project runs at another rate the profile WRITE correctly fails closed and the
gate verifies that instead (set the project to 48000 to author a real master).

Usage:
    python3 verify_o3.py                 # auto-find reaper_mcp.json; leaves scratch tracks + temp files
    python3 verify_o3.py --cleanup       # also delete scratch tracks + the temp ADM files
    python3 verify_o3.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 158  # O3 adds analysis.adm_profile_check (export_adm profile mode = a param, not a tool)
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]
ADM_PROFILE = "/tmp/mcp_o3_adm_profile.wav"
ADM_RAW = "/tmp/mcp_o3_adm_raw.wav"

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

def vcodes(result):
    return {v.get("code") for v in result.get("violations", []) if isinstance(v, dict)}

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_o3", "version": "0"}})

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
      "tool surface == %d (O3 adds adm_profile_check)" % EXPECTED_SURFACE,
      "count=%s" % enum.get("count"))

base = call("project.get_summary").get("trackCount", 0)

# scratch: a 7.1.2 bed (10 ch) + two mono objects. objA gets an IEM panner (readable position).
bed = add_track("MCP O3 bed 7.1.2")
call("spatial.set_track_channels", track=bed, channels=10)
toneBed = add_tone(bed)
objA = add_track("MCP O3 obj A")
toneA = add_tone(objA)
panOk = not failed_closed(raw("spatial.ambisonic_encode", track=objA, azimuthDeg=30.0,
                              elevationDeg=0.0, order=1))
objB = add_track("MCP O3 obj B")
add_tone(objB)
if not (toneBed and toneA):
    notes.append("no stock tone JSFX — audio is silence; conformance asserts still exercised.")
if not panOk:
    notes.append("spatial.ambisonic_encode unavailable — objA placed front-center (conformance unaffected).")

# O2-style non-conformant metadata: divergence + importance + unequal (degree) extent. The profile
# normalizer must strip/collapse ALL of it; a non-profile export must carry it (and be flagged).
META = [
    {"track": objA, "width": 45.0, "height": 20.0, "depth": 0.2, "divergence": 0.5, "importance": 8},
    {"track": objB, "size": 0.3, "importance": 3},
]

# ================================================================================================
print("\n== 1. export_adm profile:dolby-atmos — dryRun plan ==")
dry = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           objectMetadata=META, profile="dolby-atmos", dryRun=True, **BOUNDS)
prof = dry.get("profile") or {}
check(prof.get("conformant") is True, "dryRun profile block conformant", "profile=%s" % prof)
check(dry.get("coordinateMode") == "cartesian", "profile forces cartesian coordinate mode",
      "coordinateMode=%s" % dry.get("coordinateMode"))
norm = prof.get("normalized", [])
check(len(norm) >= 2, "normalized[] records the stripped/collapsed deviations",
      "normalized=%d entries" % len(norm))

# ================================================================================================
print("\n== 2. raw (non-profile) export is flagged non-conformant ==")
rawexp = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
              objectMetadata=META, outPath=ADM_RAW, **BOUNDS)
sr = ((call("analysis.adm_inspect", path=ADM_RAW).get("format") or {}).get("sampleRate"))
check(rawexp.get("ok") is True and os.path.exists(ADM_RAW), "raw O2 ADM written", "sr=%s" % sr)
rawchk = call("analysis.adm_profile_check", path=ADM_RAW)
check(rawchk.get("conformant") is False, "raw O2 file is NON-conformant")
codes = vcodes(rawchk)
check({"divergence", "importance", "coordinate", "interpolation"}.issubset(codes),
      "raw file flags divergence/importance/coordinate/interpolation", "codes=%s" % sorted(codes))

# ================================================================================================
print("\n== 3. profile write is conformant end-to-end (needs a 48 kHz project) ==")
is48k = (sr == 48000)
if is48k:
    exp = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
               objectMetadata=META, profile="dolby-atmos", outPath=ADM_PROFILE, **BOUNDS)
    check(exp.get("ok") is True and os.path.exists(ADM_PROFILE), "profile ADM written",
          "path=%s" % exp.get("path"))
    chk = call("analysis.adm_profile_check", path=ADM_PROFILE)
    check(chk.get("conformant") is True, "profile file passes adm_profile_check",
          "violations=%s" % chk.get("violations"))
    # prove the normalization reached the bytes: adm_inspect shows the O2 metadata STRIPPED.
    adm = call("analysis.adm_inspect", path=ADM_PROFILE).get("adm", {})
    check(adm.get("hasObjectDivergence") is False, "written file: objectDivergence STRIPPED")
    check(adm.get("hasImportance") is False, "written file: importance STRIPPED")
    check(adm.get("coordinateMode") == "cartesian", "written file: objects cartesian")
else:
    notes.append("project sample rate = %s (not 48000) — profile write correctly fails closed." % sr)
    fc = raw("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
             objectMetadata=META, profile="dolby-atmos", outPath=ADM_PROFILE, **BOUNDS)
    check(err_code(fc) == "sample_rate_not_48k",
          "non-48k project: profile write fails closed (sample_rate_not_48k)", "err=%s" % err_code(fc))

# ================================================================================================
print("\n== 4. fail-closed: profile on a non-Atmos bed ==")
badbed = raw("spatial.export_adm", bedTrack=bed, bedLayout="7.1.4", objectTracks=[objA],
             profile="dolby-atmos", dryRun=True, **BOUNDS)
check(err_code(badbed) == "bed_not_profile_conformant",
      "7.1.4 bed under profile fails closed (bed_not_profile_conformant)", "err=%s" % err_code(badbed))

miss = raw("analysis.adm_profile_check", path="/tmp/_mcp_no_such_o3.wav")
check(err_code(miss) == "file_not_found", "adm_profile_check on a missing file -> file_not_found")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for f in (ADM_PROFILE, ADM_RAW):
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
    notes.append("scratch tracks + %s / %s left in place (pass --cleanup)." % (ADM_PROFILE, ADM_RAW))

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
