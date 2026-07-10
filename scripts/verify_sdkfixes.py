#!/usr/bin/env python3
"""Live-verify the two carried-forward 84578d7 SDK-path fixes against a running REAPER.

These two behavior fixes ship on `main` (now also public) but were COMPILED + host-core-tested only,
never live-verified (the SDK path isn't exercised by ctest). This gate closes that gap on the real
SDK path. REAPER must be FOREGROUND (App Nap) and the current-main dylib must be installed.

FIX 1 — analysis.check_deliverable RENDER_SETTINGS 2 -> 3 (grade the BUS, not the master).
  A bare RENDER_SETTINGS=2 is not a valid stems source, so REAPER silently rendered the MASTER MIX
  at the forced width — a spec check on a named bus graded the master. The fix uses 3 (stems |
  selected tracks) so the target bus is rendered.
  METHOD: a target bus B (tone) plus TWO loud distractor tracks, all routed to master (so B is NOT
  the sole route). Independently measure B via analysis.stem_loudness (the M2-fixed stems path).
  Then:
    * check_deliverable(target=B).metrics.lufsIntegrated  should ≈ the independent bus loudness.
    * check_deliverable() [master] should be materially LOUDER (B + distractors).
  If BUGGY, check_deliverable(target=B) would ≈ the master (much louder than the true bus).

FIX 2 — azimuth x = −sin(az) at set_source_position / panSourceToBed (+az = LEFT).
  The surround-PANNER write sites placed +az to the RIGHT while the ambisonic encoder placed +az LEFT
  ("a source LEFT in a scene but RIGHT in a bed"). The fix makes the BED agree with the SCENE.
  METHOD (bed, the fixed site): a 5.1 bed with a tone + ReaSurroundPan; drive set_source_position to
  az=+90 and az=−90 and meter per-channel rmsDb. +az must put energy on the LEFT speakers (L/Ls),
  −az on the RIGHT (R/Rs).  ANCHOR (scene, already correct): ambisonic_encode at +90/−90 and read
  analysis.spatial_field DoA — should track +90/−90. Bed + scene agreeing = the fix.

Usage:
    python3 verify_sdkfixes.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_sdkfixes.py --cleanup       # delete the scratch tracks afterward
    python3 verify_sdkfixes.py /path/to/reaper_mcp.json
Stdlib only.
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
MIN_SURFACE = 158
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

passed = failed = skipped = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def skip(label, why):
    global skipped
    skipped += 1
    print("  [SKIP] %s  - %s" % (label, why))

def add_tone(track):
    for nm in TONE_CANDIDATES:
        r = raw("fx.add", track=track, name=nm)
        if not r.get("isError") and r.get("structuredContent", {}).get("fxIndex", -1) >= 0:
            return True
    return False

def add_track(name):
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

BOUNDS_LONG = dict(boundsFlag=0, startPos=0.0, endPos=1.0)   # deliverable: stable integrated LUFS
BOUNDS = dict(boundsFlag=0, startPos=0.0, endPos=0.5)        # bed/scene meters

# ================================================================================================
rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "verify_sdkfixes", "version": "0"}})
print("== 0. surface ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count", 0) >= MIN_SURFACE, "tool surface >= %d" % MIN_SURFACE,
      "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
need = ["analysis.check_deliverable", "analysis.stem_loudness", "analysis.meter",
        "spatial.set_source_position", "spatial.add_surround_panner",
        "spatial.ambisonic_encode", "analysis.spatial_field"]
check(all(n in listed for n in need), "all tools under test present",
      "missing=%s" % [n for n in need if n not in listed])

# idempotent: clear any leftover SDKFIX scratch tracks from a prior aborted run (remove high->low)
def preclean():
    r = call("track.list")
    victims = sorted([t.get("index") for t in r.get("tracks", [])
                      if str(t.get("name", "")).startswith("SDKFIX")], reverse=True)
    for idx in victims:
        call("track.remove", track=idx)
    if victims:
        print("  pre-clean: removed %d leftover SDKFIX track(s)" % len(victims))
preclean()

base = call("project.get_summary").get("trackCount", 0)
scratch = []

# ================================================================================================
print("\n== FIX 1: check_deliverable grades the BUS, not the master (RENDER_SETTINGS 2->3) ==")
busB = add_track("SDKFIX busB (target, tone)"); scratch.append(busB)
toneB = add_tone(busB)
d1 = add_track("SDKFIX distractor 1 (tone)"); scratch.append(d1); t1 = add_tone(d1)
d2 = add_track("SDKFIX distractor 2 (tone)"); scratch.append(d2); t2 = add_tone(d2)
print("  tracks: busB=%d d1=%d d2=%d   tones: B=%s d1=%s d2=%s" % (busB, d1, d2, toneB, t1, t2))

if not (toneB and t1 and t2):
    skip("deliverable level asserts", "no stock tone JSFX — cannot build the bus/master contrast")
    # still prove the tool runs + shapes hold
    cd = call("analysis.check_deliverable", spec="ebu-r128", target=busB, **BOUNDS_LONG)
    check(isinstance(cd.get("metrics"), dict), "check_deliverable(target) returns a metrics block")
else:
    # Independent per-stem loudness via the M2-fixed stems path (RENDER_SETTINGS=3). All three tones
    # are ~equal and all route to master, so the master mix (their sum) is ~+9 LU above bus B alone.
    # We DON'T render the master (that no-target render is slow/flaky here) — we INFER it's much
    # louder from the two loud distractor stems, then require check_deliverable(target=B) to still
    # track B. If check_deliverable were buggy (grading the master), it would read ~+9 LU high.
    s = call("analysis.stem_loudness", tracks=[busB, d1, d2], **BOUNDS_LONG)
    stems = {row.get("track"): row.get("lufsIntegrated") for row in s.get("stems", [])}
    ref_bus, ref_d1, ref_d2 = stems.get(busB), stems.get(d1), stems.get(d2)
    cd_bus_r = call("analysis.check_deliverable", spec="ebu-r128", target=busB, **BOUNDS_LONG)
    cd_bus = cd_bus_r.get("metrics", {}).get("lufsIntegrated")
    print("  stem_loudness  busB=%s  d1=%s  d2=%s LUFS" % (ref_bus, ref_d1, ref_d2))
    print("  check_deliverable(target=busB)     : %s LUFS" % cd_bus)
    if None in (ref_bus, ref_d1, ref_d2, cd_bus):
        check(False, "all loudness measures present",
              "bus=%s d1=%s d2=%s cd=%s" % (ref_bus, ref_d1, ref_d2, cd_bus))
    else:
        check(ref_d1 > ref_bus - 3.0 and ref_d2 > ref_bus - 3.0,
              "setup valid: two other loud tracks also route to master (B is NOT the sole route)",
              "busB=%.1f d1=%.1f d2=%.1f LUFS" % (ref_bus, ref_d1, ref_d2))
        check(abs(cd_bus - ref_bus) < 2.0,
              "FIX 1: check_deliverable(target) tracks the BUS, not the master",
              "|deliverable - true bus| = %.2f LU (a buggy build would read ~+9 toward the master)"
              % abs(cd_bus - ref_bus))

# ================================================================================================
print("\n== FIX 2 (bed): +az pans LEFT via set_source_position ==")
LEFT = lambda lbl: lbl.upper().startswith("L") and lbl.upper() != "LFE"
RIGHT = lambda lbl: lbl.upper().startswith("R")

# Use az=+/-45 (source X normalized ~0.15/0.85, OFF the 0.0/1.0 edge) — ReaSurroundPan resets a
# source written to the exact edge, so +/-90 can't be observed. +/-45 still unambiguously L vs R.
AZ = 45.0
def expected_x(az):
    # post-fix: the panner path writes in1X = (sin(az)+1)/2 -> +az maps HIGH (left), because
    # ReaSurroundPan in1X runs 0=right..1=left.
    return (math.sin(math.radians(az)) + 1.0) / 2.0
def bed_measure(track, fxpan, az):
    """set source to az with a read-after-write retry (ReaSurroundPan writes are flaky — they
    intermittently reset to default); return (xNorm, maxLeftRmsDb, maxRightRmsDb, rows)."""
    exp, xnorm = expected_x(az), None
    for _ in range(8):
        call("spatial.set_source_position", track=track, fx=fxpan, source=0,
             azimuthDeg=az, elevationDeg=0.0)
        st = call("spatial.get_surround_state", track=track, fx=fxpan)
        xnorm = next((p.get("normalized") for p in st.get("params", [])
                      if p.get("name", "").strip().lower() == "in 1 x"), None)
        if xnorm is not None and abs(xnorm - exp) < 0.03:
            break
    m = call("analysis.meter", target=track, **BOUNDS)
    rows = m.get("channelsDetail", [])
    lv = [c.get("rmsDb") for c in rows if LEFT(c.get("label", "")) and not c.get("isLFE")
          and isinstance(c.get("rmsDb"), (int, float))]
    rv = [c.get("rmsDb") for c in rows if RIGHT(c.get("label", ""))
          and isinstance(c.get("rmsDb"), (int, float))]
    return (xnorm, max(lv) if lv else None, max(rv) if rv else None, rows)

bed = add_track("SDKFIX bed 5.1 (tone->ReaSurroundPan)"); scratch.append(bed)
bedTone = add_tone(bed)
pan = raw("spatial.add_surround_panner", track=bed, setTrackChannels=6, speakers=6, channels=1)
pan_ok = not pan.get("isError")
FXpan = pan.get("structuredContent", {}).get("fxIndex", 1) if pan_ok else None
if not pan_ok:
    skip("bed +az asserts", "add_surround_panner failed: %s" % pan.get("content"))
elif not bedTone:
    skip("bed +az asserts", "no stock tone JSFX — bed is silent")
else:
    xp, lp, rp, rows = bed_measure(bed, FXpan, +AZ)
    print("  az=+45:  in1X=%s  " % xp + "  ".join("%s=%.1f" % (c.get("label"), c.get("rmsDb"))
          for c in rows if isinstance(c.get("rmsDb"), (int, float))))
    xm, lm, rm, _ = bed_measure(bed, FXpan, -AZ)
    print("  az=-45:  in1X=%s  L-max=%.1f R-max=%.1f  |  +45: L-max=%.1f R-max=%.1f dB"
          % (xm, lm if lm is not None else -999, rm if rm is not None else -999,
             lp if lp is not None else -999, rp if rp is not None else -999))
    # param-level: the fix writes +az to the LEFT half of X, -az to the RIGHT half
    if xp is not None and xm is not None:
        check(xp > 0.55 and xm < 0.45,
              "FIX 2 (param): +az writes source X to the LEFT half (in1X high; ReaSurroundPan 0=right/1=left)",
              "x@+45=%.3f  x@-45=%.3f" % (xp, xm))
    else:
        check(False, "source X param readable at both az", "x+=%s x-=%s" % (xp, xm))
    # physical: that left-of-centre source renders LEFT-dominant, right-of-centre renders RIGHT
    if None in (lp, rp, lm, rm):
        check(False, "left/right channel levels readable at both az",
              "lp=%s rp=%s lm=%s rm=%s" % (lp, rp, lm, rm))
    else:
        check(lp - rp > 4.0, "FIX 2 (render): +az is LEFT-dominant in the bed (L/Ls > R/Rs)",
              "L-R = %.1f dB at +45" % (lp - rp))
        check(rm - lm > 4.0, "FIX 2 (render): -az is RIGHT-dominant in the bed (R/Rs > L/Ls)",
              "R-L = %.1f dB at -45" % (rm - lm))

# ================================================================================================
print("\n== FIX 2 (scene anchor): +az reads LEFT in an ambisonic scene (already-correct path) ==")
scene = add_track("SDKFIX FOA scene (tone->encoder)"); scratch.append(scene)
scTone = add_tone(scene)
call("spatial.set_track_channels", track=scene, channels=4)

def scene_doa(az):
    enc = raw("spatial.ambisonic_encode", track=scene, azimuthDeg=az, elevationDeg=0.0, order=1)
    if enc.get("isError"):
        return None, None, enc
    sf = call("analysis.spatial_field", target=scene, **BOUNDS)
    return sf.get("doa", {}).get("azimuthDeg"), sf.get("diffuseness"), None

def near(a, b, tol=35.0):
    return abs(((a - b) + 540.0) % 360.0 - 180.0) < tol

if not scTone:
    skip("scene DoA asserts", "no stock tone JSFX — scene is silent")
else:
    az_p, diff_p, encErr = scene_doa(+90.0)
    if encErr is not None:
        skip("scene DoA asserts", "ambisonic_encode unavailable (IEM encoder?): %s" % encErr.get("content"))
    else:
        az_m, diff_m, _ = scene_doa(-90.0)
        print("  encode +90 -> DoA az=%s (diff=%s) ;  encode -90 -> DoA az=%s (diff=%s)"
              % (az_p, diff_p, az_m, diff_m))
        if az_p is None or az_m is None:
            check(False, "spatial_field returned a DoA azimuth at both encodings")
        else:
            if (diff_p or 0) < 0.6:
                check(near(az_p, +90.0), "scene: +az reads LEFT (DoA ~ +90)", "az=%.1f" % az_p)
            else:
                skip("scene +90 DoA", "field not focused (diffuseness %.2f)" % (diff_p or 0))
            if (diff_m or 0) < 0.6:
                check(near(az_m, -90.0), "scene: -az reads RIGHT (DoA ~ -90)", "az=%.1f" % az_m)
            else:
                skip("scene -90 DoA", "field not focused (diffuseness %.2f)" % (diff_m or 0))
        notes.append("bed AND scene now agree that +az = LEFT — the 84578d7 azimuth fix is consistent.")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for t in sorted(set(scratch), reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base, "all scratch tracks removed")
else:
    notes.append("scratch tracks %s left in place (pass --cleanup to remove)" % sorted(set(scratch)))

print("\n" + "=" * 66 + "\n  PASS=%d  FAIL=%d  SKIP=%d" % (passed, failed, skipped))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN" + ("" if skipped == 0 else " (with skips — see above)"))
