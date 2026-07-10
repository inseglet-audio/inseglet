#!/usr/bin/env python3
"""Live-verify Batch M2 — metering depth (4 tools + spatial_field timeline) against a running REAPER.

Exercises, end-to-end on the real SDK path:
  * analysis.stem_loudness    — per-track BS.1770-4 gated loudness + dBTP in ONE multi-stem pass.
  * analysis.dialog_loudness  — program loudness (RENDER_STATS) + speech-active dialog-stem loudness
                                (C++ gated on the isolated stem) + program-to-dialog offset.
  * analysis.downmix_check    — BS.775 stereo/mono fold-down QC computed from one bit-exact render.
  * analysis.loudness_timeline— momentary/short-term series + maxima positions + LRA detail.
  * analysis.spatial_field    — the M2 timeline mode (windowed DoA trajectory).

The DSP itself (gating, windowing, LRA percentiles, downmix matrices, windowed DirAC) is proven
DETERMINISTICALLY off-REAPER in `unit.meter`. This gate proves the TOOLS run end-to-end on real REAPER
renders and that the read-only guards hold: it builds its own scratch tracks, runs everything bounded,
and checks shapes + guards. Audio content (a stock tone JSFX) and an ambisonic encode are BEST-EFFORT:
without them the plumbing still runs and level/direction asserts are noted as skipped. Nothing opens a
dialog. REAPER must be FOREGROUND, and `cmake --install build` must have shipped the current dylib.

Usage:
    python3 verify_m2.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_m2.py --cleanup       # also delete the scratch tracks it created
    python3 verify_m2.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 151  # 147 (Batch M1) + 4 (Batch M2)
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

def err_text(result):
    return " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))

def err_code(result):
    """Structured-error code of a RETURNED makeError (read-only analysis idiom: MCP isError stays
    false — the dispatcher only sets isError on a THROW/timeout), else None."""
    sc = result.get("structuredContent", {})
    return sc.get("error") if isinstance(sc, dict) else None

def failed_closed(result):
    return result.get("isError") is True or err_code(result) is not None

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_m2", "version": "0"}})

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

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (147 + 4 M2)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
m2 = ["analysis.stem_loudness", "analysis.dialog_loudness",
      "analysis.downmix_check", "analysis.loudness_timeline"]
check(all(n in listed for n in m2), "all four M2 tools present in tools/list",
      "missing=%s" % [n for n in m2 if n not in listed])

base = call("project.get_summary").get("trackCount", 0)

# scratch content: a toned stereo stem, a silent stereo stem, a 5.1 bed with tone, a FOA scene
stemA = add_track("MCP M2 stem A (tone)")
toneA = add_tone(stemA)
stemB = add_track("MCP M2 stem B (silent)")
bed = add_track("MCP M2 bed 5.1")
call("spatial.set_track_channels", track=bed, channels=6)
toneBed = add_tone(bed)
if not (toneA and toneBed):
    notes.append("no stock tone JSFX — level asserts skipped where noted (plumbing still exercised).")
# LIVE-GATE FINDING (M2): RENDER_BOUNDSFLAG 1 = ENTIRE PROJECT (not time selection). Bound every
# render with a CUSTOM range (boundsFlag=0 + startPos/endPos) so the gate is deterministic on any
# open project.
BOUNDS = dict(boundsFlag=0, startPos=0.0, endPos=0.5)

# ================================================================================================
print("\n== 1. analysis.stem_loudness — one pass, per-stem gated LUFS + balance ==")
bad = raw("analysis.stem_loudness")
check(bad.get("isError") is True, "missing tracks arg -> isError (required-arg contract)")

dry = call("analysis.stem_loudness", tracks=[stemA, stemB], dryRun=True)
check(dry.get("dryRun") is True and dry.get("count") == 2 and "plan" in dry,
      "stem_loudness dryRun: plan + 2 stems, no render")

dup = raw("analysis.stem_loudness", tracks=[stemA, stemA])
check(failed_closed(dup) and err_code(dup) == "duplicate_stem",
      "duplicate stem -> fails closed (duplicate_stem)", "error=%s" % err_code(dup))

sl = call("analysis.stem_loudness", tracks=[stemA, stemB], **BOUNDS)
stems = sl.get("stems", [])
check(len(stems) == 2 and sl.get("count") == 2, "two per-stem rows returned",
      "count=%s rows=%d" % (sl.get("count"), len(stems)))
fields_ok = all(all(k in s for k in ("lufsIntegrated", "ungatedLufs", "activityFraction",
                                     "truePeakDb", "deltaFromLoudestLu")) for s in stems)
check(fields_ok, "every stem row has gated/ungated/activity/truePeak/delta")
if len(stems) == 2:
    byTrack = {s["track"]: s for s in stems}
    a, b = byTrack.get(stemA, {}), byTrack.get(stemB, {})
    check(isinstance(sl.get("loudest"), dict) and
          any(abs(s.get("deltaFromLoudestLu", 99)) < 0.01 for s in stems),
          "loudest stem reported with delta 0")
    if toneA:
        check(a.get("lufsIntegrated", -999) > -70.0, "toned stem measures above the -70 gate",
              "lufs=%s" % a.get("lufsIntegrated"))
        check(b.get("lufsIntegrated", 0) < a.get("lufsIntegrated", 0),
              "silent stem measures below the toned stem",
              "silent=%s toned=%s" % (b.get("lufsIntegrated"), a.get("lufsIntegrated")))

# ================================================================================================
print("\n== 2. analysis.dialog_loudness — program vs dialog stem ==")
bad = raw("analysis.dialog_loudness")
check(bad.get("isError") is True, "missing dialogTrack -> isError (required-arg contract)")

mrej = raw("analysis.dialog_loudness", dialogTrack="master")
check(failed_closed(mrej) and err_code(mrej) == "invalid_dialog_target",
      "dialogTrack='master' -> fails closed (invalid_dialog_target)", "error=%s" % err_code(mrej))

dry = call("analysis.dialog_loudness", dialogTrack=stemA, dryRun=True)
check(dry.get("dryRun") is True and "plan" in dry and dry.get("dialogTrack") == stemA,
      "dialog_loudness dryRun: two-render plan, no render")

dl = call("analysis.dialog_loudness", dialogTrack=stemA, **BOUNDS)
dlg = dl.get("dialog", {})
prog = dl.get("program", {})
check(isinstance(dlg, dict) and isinstance(prog, dict) and "method" in dl,
      "dialog_loudness returns program + dialog + method")
af = dlg.get("activityFraction")
check(isinstance(af, (int, float)) and 0.0 <= af <= 1.0, "dialog activity fraction in [0,1]",
      "activity=%s" % af)
if toneA:
    check(dlg.get("lufsIntegrated", -999) > -70.0,
          "dialog stem gated loudness above the -70 gate", "lufs=%s" % dlg.get("lufsIntegrated"))
    off = dl.get("programToDialogLu")
    check(off is None or isinstance(off, (int, float)),
          "program-to-dialog offset is a number (or null when unmeasurable)", "offset=%s" % off)

# ================================================================================================
print("\n== 3. analysis.downmix_check — BS.775 fold QC ==")
quad = add_track("MCP M2 quad (unsupported)")
call("spatial.set_track_channels", track=quad, channels=4)
un = raw("analysis.downmix_check", target=quad)
check(failed_closed(un) and err_code(un) == "downmix_unsupported",
      "4-ch (unlabeled) width -> fails closed (downmix_unsupported)", "error=%s" % err_code(un))

dry = call("analysis.downmix_check", target=bed, dryRun=True)
coef = dry.get("coefficients", [])
check(dry.get("dryRun") is True and len(coef) == 6, "downmix dryRun echoes 6 coefficient rows")
if len(coef) == 6:
    c_row = next((c for c in coef if c.get("label") == "C"), {})
    lfe_row = next((c for c in coef if c.get("label") == "LFE"), {})
    check(abs(c_row.get("toLo", 0) - 0.7071) < 0.001 and abs(c_row.get("toRo", 0) - 0.7071) < 0.001,
          "centre folds at -3 dB to both sides")
    check(lfe_row.get("toLo", 1) == 0 and lfe_row.get("toRo", 1) == 0,
          "LFE omitted from the fold by default")

dm = call("analysis.downmix_check", target=bed, **BOUNDS)
check(all(k in dm for k in ("multichannel", "stereo", "mono", "deltas", "interpretation")),
      "downmix_check returns multichannel/stereo/mono/deltas/interpretation")
corr = dm.get("stereo", {}).get("correlation")
check(isinstance(corr, (int, float)) and -1.0 <= corr <= 1.0, "Lo/Ro correlation in [-1,1]",
      "corr=%s" % corr)
if toneBed:
    check(dm.get("stereo", {}).get("lufsIntegrated", -999) > -70.0,
          "stereo fold of the toned bed measures above the -70 gate",
          "lufs=%s" % dm.get("stereo", {}).get("lufsIntegrated"))

# ================================================================================================
print("\n== 4. analysis.loudness_timeline — series + maxima + LRA ==")
tl = call("analysis.loudness_timeline", target=bed, hopSec=0.05, **BOUNDS)
times, mom, st = tl.get("times", []), tl.get("momentary", []), tl.get("shortTerm", [])
check(len(times) > 0 and len(times) == len(mom) == len(st) == tl.get("points"),
      "aligned times/momentary/shortTerm series", "points=%s" % tl.get("points"))
mm = tl.get("maxMomentary", {})
check("lufs" in mm and "timeSec" in mm and "lra" in tl,
      "maxima positions + LRA detail present")
if toneBed and mom:
    check(max(mom) > -70.0, "momentary series sees the tone", "maxM=%.1f" % max(mom))
    check(abs(mm.get("lufs", -999) - max(mom)) < 0.05, "reported max == series max")

# guard: a sub-400ms range cannot form one loudness block
short = raw("analysis.loudness_timeline", target=bed, boundsFlag=0, startPos=0.0, endPos=0.2)
check(failed_closed(short) and err_code(short) == "range_too_short",
      "sub-400ms range -> fails closed (range_too_short)", "error=%s" % err_code(short))

# ================================================================================================
print("\n== 5. analysis.spatial_field — M2 timeline (windowed DoA trajectory) ==")
scene = add_track("MCP M2 scene")
scene_tone = add_tone(scene)
encoded = False
enc = raw("spatial.ambisonic_encode", track=scene, azimuthDeg=90.0, elevationDeg=0.0, order=1)
if not failed_closed(enc):
    encoded = True
else:
    call("spatial.set_track_channels", track=scene, channels=4)
    notes.append("spatial.ambisonic_encode unavailable — trajectory direction not asserted.")

sf = call("analysis.spatial_field", target=scene, timeline=True,
          windowSec=0.2, hopSec=0.1, **BOUNDS)
tlf = sf.get("timeline", {})
wins = tlf.get("windows", [])
check(isinstance(tlf, dict) and len(wins) >= 2,
      "field timeline returns >=2 windows", "windows=%d" % len(wins))
if wins:
    w0 = wins[0]
    check(all(k in w0 for k in ("t", "azimuthDeg", "elevationDeg", "diffuseness", "rV", "rE")),
          "each window has t/az/el/diffuseness/rV/rE")
    if encoded and scene_tone:
        diff0 = w0.get("diffuseness", 1.0)
        az0 = w0.get("azimuthDeg", 0.0)
        if diff0 < 0.6:
            d = abs(((az0 - 90.0) + 540.0) % 360.0 - 180.0)
            check(d < 35.0, "windowed DoA near the encoded 90 deg",
                  "az=%.1f diff=%.2f" % (az0, diff0))
        else:
            notes.append("encoded field not focused in windows (diff=%.2f) — direction not asserted."
                         % diff0)

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for t in sorted([stemA, stemB, bed, quad, scene], reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base, "all scratch tracks removed")
else:
    notes.append("scratch tracks %s left in place (pass --cleanup to remove)"
                 % sorted([stemA, stemB, bed, quad, scene]))

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
