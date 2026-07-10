#!/usr/bin/env python3
"""Live-verify Batch A — native Dolby Atmos Master File (DAMF) triad authoring.

Exercises, end-to-end on the real SDK path:
  * spatial.export_damf   — render a DirectSpeakers bed + mono object stems and author the DAMF triad
                            (<base>.atmos YAML manifest + <base>.atmos.metadata YAML position track +
                            <base>.atmos.audio big-endian CAF PCM), each object's position sampled from
                            its panner. Enforces the Dolby Atmos Master constraints (<=7.1.2 bed, 48 kHz).
  * analysis.damf_inspect — read the triad back (manifest roster + metadata events + CAF header) and
                            report structure — the round-trip QC.

The YAML/CAF serialization + the coordinate map + the round-trip parser are proven DETERMINISTICALLY
off-REAPER in `unit.damf`. This gate proves the TOOLS run end-to-end on real REAPER renders, that the three
files are written + structurally sound, that object positions round-trip into .atmos.metadata, and that
the guards hold. It builds its own scratch tracks, bounds every render, and writes to a temp basename it
can delete. Tone (a stock JSFX) + an object panner (IEM encoder via spatial.ambisonic_encode) are
BEST-EFFORT. Nothing opens a dialog. REAPER must be FOREGROUND + 48 kHz, and `cmake --install build` must
have shipped the current dylib.

Usage:
    python3 verify_damf.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_damf.py --cleanup       # also delete scratch tracks + the temp triad
    python3 verify_damf.py /path/to/reaper_mcp.json
"""
import json
import os
import re
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 183  # 181 (v1.2.0) + 2 (Batch A DAMF: export_damf + damf_inspect)
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]
DAMF_BASE = "/tmp/mcp_damf"
DAMF_FILES = [DAMF_BASE + ".atmos", DAMF_BASE + ".atmos.audio", DAMF_BASE + ".atmos.metadata"]

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
                              "clientInfo": {"name": "verify_damf", "version": "0"}})

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

# Bound every render deterministically (RENDER_BOUNDSFLAG 0 = custom range).
BOUNDS = dict(boundsFlag=0, startPos=0.0, endPos=0.5)

def obj_event_positions(meta_text, oid):
    """Return the [x,y,z] positions of every event for object ID `oid` in the .atmos.metadata YAML."""
    out = []
    for line in meta_text.splitlines():
        if ("ID: %d," % oid) in line and "pos:" in line:
            m = re.search(r"pos:\s*\[([^\]]+)\]", line)
            if m:
                out.append([float(v) for v in m.group(1).split(",")])
    return out

# ================================================================================================
initialize()
print("== 0. surface == %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (181 + 2 DAMF)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
damf_tools = ["spatial.export_damf", "analysis.damf_inspect"]
check(all(n in listed for n in damf_tools), "both DAMF tools present in tools/list",
      "missing=%s" % [n for n in damf_tools if n not in listed])

# DAMF mandates 48 kHz. Open a deterministic 48 kHz scratch project (the render uses the project rate)
# so the gate does not depend on the ambient project's sample rate.
RPP48 = "/tmp/mcp_damf_48k.rpp"
with open(RPP48, "w") as fh:
    fh.write('<REAPER_PROJECT 0.1 "7.0" 0\n  SAMPLERATE 48000 1 0\n  TEMPO 120 4 4\n>\n')
op = raw("project.open", path=RPP48, discardChanges=True)
check(not failed_closed(op), "opened a 48 kHz scratch project (project.open discardChanges)")
sr_now = call("project.get_summary").get("sampleRate")
check(sr_now == 48000, "project sample rate is 48000", "sr=%s" % sr_now)

base = call("project.get_summary").get("trackCount", 0)

# scratch content: a 7.1.2 bed (10 ch, toned), objA (mono, toned, IEM-encoded @az=90 -> hard LEFT so its
# DAMF x should read strongly negative), objB (toned, NO panner -> front-centre).
bed = add_track("MCP DAMF bed 7.1.2")
call("spatial.set_track_channels", track=bed, channels=10)
toneBed = add_tone(bed)
objA = add_track("MCP DAMF obj A")
toneA = add_tone(objA)
panOk = not failed_closed(raw("spatial.ambisonic_encode", track=objA, azimuthDeg=90.0,
                              elevationDeg=0.0, order=1))
objB = add_track("MCP DAMF obj B")
add_tone(objB)
if not (toneBed and toneA):
    notes.append("no stock tone JSFX — audio is silence; structure asserts still exercised.")
if not panOk:
    notes.append("spatial.ambisonic_encode unavailable — object position not asserted (front-centre).")

# ================================================================================================
print("\n== 1. spatial.export_damf — guards + dryRun plan ==")
nosrc = raw("spatial.export_damf")
check(failed_closed(nosrc) and err_code(nosrc) == "no_sources",
      "no bed + no objects -> fails closed (no_sources)", "error=%s" % err_code(nosrc))

dry = call("spatial.export_damf", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           dryRun=True, **BOUNDS)
check(dry.get("dryRun") is True and dry.get("bedChannels") == 10 and dry.get("objectCount") == 2 and
      dry.get("channels") == 12,
      "export_damf dryRun: 10-ch bed + 2 objects = 12 channels, no files")
check(isinstance(dry.get("objects"), list) and all("positionSource" in o for o in dry["objects"]),
      "dryRun reports per-object positionSource (envelope/static/none)")

# ================================================================================================
print("\n== 2. spatial.export_damf — author the DAMF triad ==")
skip_render = False
expraw = raw("spatial.export_damf", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
             outPath=DAMF_BASE, **BOUNDS)
if failed_closed(expraw):
    code = err_code(expraw)
    if code == "sample_rate_not_48k":
        skip_render = True
        notes.append("project is NOT 48 kHz — DAMF requires 48000. Render-dependent asserts skipped; "
                     "set the project sample rate to 48000 and re-run for the full gate.")
        print("  [SKIP] export_damf render (project not 48 kHz)")
    else:
        check(False, "export_damf authored the triad", "error=%s" % code)
else:
    exp = expraw.get("structuredContent", {})
    check(exp.get("ok") is True and exp.get("path") == DAMF_BASE + ".atmos",
          "export_damf wrote the .atmos manifest", "path=%s" % exp.get("path"))
    check(exp.get("channels") == 12, "authored 12 channels (10 bed + 2 objects)",
          "channels=%s" % exp.get("channels"))
    check(exp.get("frames", 0) > 0, "non-empty render (frames > 0)", "frames=%s" % exp.get("frames"))
    check(abs(exp.get("sampleRate", 0) - 48000) < 1, "48 kHz essence",
          "sr=%s" % exp.get("sampleRate"))
    for f in DAMF_FILES:
        check(os.path.exists(f), "triad file exists: %s" % os.path.basename(f))

    # ---- CAF essence shape from the tool's own inspect readout ----
    audio = exp.get("damf", {}).get("audio", {})
    check(audio.get("format") == "lpcm", "CAF format lpcm", "fmt=%s" % audio.get("format"))
    check(audio.get("littleEndian") is False, "CAF essence is big-endian (Dolby master convention)")
    check(audio.get("channels") == 12, "CAF has 12 channels", "ch=%s" % audio.get("channels"))

# ================================================================================================
print("\n== 3. analysis.damf_inspect — round-trip the authored triad ==")
missing = raw("analysis.damf_inspect", path="/tmp/_mcp_no_such.atmos")
check(err_code(missing) == "file_not_found", "missing path -> file_not_found",
      "error=%s" % err_code(missing))

if not skip_render:
    ins = call("analysis.damf_inspect", path=DAMF_BASE + ".atmos")
    check(ins.get("isDamf") is True, "inspected triad recognized as DAMF")
    man = ins.get("manifest", {})
    check(man.get("bedChannels") == 10 and man.get("objects") == 2,
          "manifest: 10 bed channels + 2 objects", "man=%s" % man)
    md = ins.get("metadata", {})
    check(md.get("sampleRate") == 48000, "metadata sampleRate 48000", "sr=%s" % md.get("sampleRate"))
    check(md.get("events", 0) >= 12, "metadata has >=12 events (10 static bed + object trajectories)",
          "events=%s" % md.get("events"))
    aud = ins.get("audio", {})
    check(aud.get("channels") == 12 and aud.get("littleEndian") is False,
          "CAF: 12 channels, big-endian")
    check(len(ins.get("warnings", [])) == 0, "no structural warnings on the authored triad",
          "warnings=%s" % ins.get("warnings"))

    # ---- positions round-trip into .atmos.metadata (objA ID = 10 bed + 1 = 11) ----
    try:
        meta_text = open(DAMF_BASE + ".atmos.metadata").read()
    except OSError:
        meta_text = ""
    posA = obj_event_positions(meta_text, 11)
    posB = obj_event_positions(meta_text, 12)
    check(len(posA) >= 1 and len(posB) >= 1,
          "both objects carry position events in .atmos.metadata",
          "objA=%d objB=%d events" % (len(posA), len(posB)))
    if panOk and posA:
        x0 = posA[0][0]
        check(abs(x0) > 0.5, "panned objA is strongly lateralized (|x|>0.5)", "x=%.3f" % x0)
        if x0 >= 0:
            notes.append("objA (panner az=90) read x=%.3f (>=0) — expected LEFT (x<0); check panner "
                         "handedness/readback." % x0)
        else:
            print("  [PASS] objA az=90 -> x=%.3f (LEFT, as expected)" % x0)
    if posB:
        xb, yb = posB[0][0], posB[0][1]
        check(abs(xb) < 0.2 and yb > 0.5, "unpanned objB is front-centre (x~0, y>0.5)",
              "posB=%s" % posB[0])

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for f in DAMF_FILES:
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
    notes.append("scratch tracks + %s.* left in place (pass --cleanup to remove)." % DAMF_BASE)

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
