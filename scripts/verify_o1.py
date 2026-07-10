#!/usr/bin/env python3
"""Live-verify Batch O1 — native ADM object-audio authoring (export_adm / author_object_bed / adm_inspect).

Exercises, end-to-end on the real SDK path:
  * spatial.author_object_bed — validate/establish the bed+object topology an ADM deliverable needs;
                                report the object map (numbers, panner presence, position) + ready flag;
                                apply mode renames objects + widens the bed in one undo block.
  * spatial.export_adm        — render the DirectSpeakers bed + the mono object stems and author a real
                                ITU-R BS.2076 ADM BWF (chna + axml + interleaved PCM), each object's
                                position sampled from its panner. Spherical AND cartesian coordinate modes.
  * analysis.adm_inspect      — parse the authored file back (chna + axml) and report container / format /
                                track table / ADM structure — the round-trip QC of the flagship.

The chna/axml/RIFF/BW64 serialization + the round-trip parser are proven DETERMINISTICALLY off-REAPER in
`unit.adm`. This gate proves the TOOLS run end-to-end on real REAPER renders, that a written ADM file is
structurally sound, and that the guards hold. It builds its own scratch tracks, bounds every render, and
writes the ADM to a temp path it deletes. Tone (a stock JSFX) and an object panner (IEM StereoEncoder via
spatial.ambisonic_encode) are BEST-EFFORT — without them the plumbing still runs and position asserts are
noted as skipped. Nothing opens a dialog. REAPER must be FOREGROUND, and `cmake --install build` must have
shipped the current dylib.

Usage:
    python3 verify_o1.py                 # auto-find reaper_mcp.json; leaves scratch tracks
    python3 verify_o1.py --cleanup       # also delete scratch tracks + the temp ADM file
    python3 verify_o1.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 157  # 154 (Batch M3) + 3 (Batch O1)
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]
ADM_OUT = "/tmp/mcp_o1_adm.wav"
ADM_OUT_CART = "/tmp/mcp_o1_adm_cart.wav"

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
                              "clientInfo": {"name": "verify_o1", "version": "0"}})

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
      "tool surface == %d (154 + 3 O1)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
o1 = ["spatial.export_adm", "spatial.author_object_bed", "analysis.adm_inspect"]
check(all(n in listed for n in o1), "all three O1 tools present in tools/list",
      "missing=%s" % [n for n in o1 if n not in listed])

base = call("project.get_summary").get("trackCount", 0)

# scratch content: a 7.1.2 bed (10 ch, toned), objA (mono, toned, IEM-encoded so it has a readable
# azimuth/elevation panner), objB (toned, NO panner -> front-center).
bed = add_track("MCP O1 bed 7.1.2")
call("spatial.set_track_channels", track=bed, channels=10)
toneBed = add_tone(bed)
objA = add_track("MCP O1 obj A")
toneA = add_tone(objA)
panOk = not failed_closed(raw("spatial.ambisonic_encode", track=objA, azimuthDeg=30.0,
                              elevationDeg=0.0, order=1))
objB = add_track("MCP O1 obj B")
add_tone(objB)
if not (toneBed and toneA):
    notes.append("no stock tone JSFX — audio is silence; structure asserts still exercised.")
if not panOk:
    notes.append("spatial.ambisonic_encode unavailable — object position not asserted (front-center).")

# ================================================================================================
print("\n== 1. spatial.author_object_bed — topology validation + object map ==")
badlay = raw("spatial.author_object_bed", bedTrack=bed, bedLayout="9.9.9")
check(failed_closed(badlay), "unknown bedLayout -> fails closed")

rep = call("spatial.author_object_bed", bedTrack=bed, bedLayout="7.1.2",
           objectTracks=[objA, objB])
check(rep.get("dryRun") is True, "author_object_bed defaults to dryRun (report only)")
bedj = rep.get("bed", {})
check(isinstance(bedj, dict) and bedj.get("layout") == "7.1.2" and bedj.get("channels") == 10 and
      len(bedj.get("speakers", [])) == 10,
      "bed reported: 7.1.2, 10 ch, 10 labelled speakers")
objsr = rep.get("objects", [])
check(len(objsr) == 2 and objsr[0].get("objectNumber") == 1 and objsr[1].get("objectNumber") == 2,
      "two objects mapped with stable 1-based numbers")
if panOk:
    check(objsr[0].get("hasPanner") is True, "objA reports a positional panner")
    check(objsr[1].get("hasPanner") is False, "objB (no panner) reported hasPanner=false")

# apply mode: rename objects (single undo block). Idempotent second run makes no new actions.
ap = call("spatial.author_object_bed", bedTrack=bed, bedLayout="7.1.2",
          objectTracks=[objA, objB], renameObjects=True, dryRun=False)
check(ap.get("dryRun") is False and isinstance(ap.get("actions"), list),
      "apply mode returns an actions list")
nm = call("track.get_name", track=objA).get("name", "")
check(nm == "Object 1", "objA renamed to 'Object 1' by apply", "name=%s" % nm)
ap2 = call("spatial.author_object_bed", bedTrack=bed, bedLayout="7.1.2",
           objectTracks=[objA, objB], renameObjects=True, dryRun=False)
check(len(ap2.get("actions", [])) == 0, "second apply is a no-op (idempotent naming)")

# ================================================================================================
print("\n== 2. spatial.export_adm — author an ADM BWF (bed + object trajectories) ==")
nosrc = raw("spatial.export_adm")
check(failed_closed(nosrc) and err_code(nosrc) == "no_sources",
      "no bed + no objects -> fails closed (no_sources)", "error=%s" % err_code(nosrc))

dry = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           dryRun=True, **BOUNDS)
check(dry.get("dryRun") is True and dry.get("bedChannels") == 10 and dry.get("objectCount") == 2 and
      dry.get("channels") == 12,
      "export_adm dryRun: 10-ch bed + 2 objects = 12 channels, no file")
check(isinstance(dry.get("objects"), list) and all("positionSource" in o for o in dry["objects"]),
      "dryRun reports per-object positionSource (envelope/static/none)")

exp = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
           outPath=ADM_OUT, **BOUNDS)
check(exp.get("ok") is True and exp.get("path") == ADM_OUT, "export_adm wrote a file",
      "path=%s" % exp.get("path"))
check(exp.get("channels") == 12 and exp.get("container") in ("RIFF/WAVE", "BW64"),
      "authored 12 channels in a RIFF/BW64 container", "container=%s" % exp.get("container"))
check(exp.get("frames", 0) > 0, "non-empty render (frames > 0)", "frames=%s" % exp.get("frames"))
check(os.path.exists(ADM_OUT), "ADM file exists on disk")

# cartesian coordinate mode
expc = call("spatial.export_adm", bedTrack=bed, bedLayout="7.1.2", objectTracks=[objA, objB],
            coordinateMode="cartesian", outPath=ADM_OUT_CART, **BOUNDS)
check(expc.get("ok") is True and expc.get("coordinateMode") == "cartesian",
      "cartesian-mode export ok")

# ================================================================================================
print("\n== 3. analysis.adm_inspect — parse the authored ADM back ==")
missing = raw("analysis.adm_inspect", path="/tmp/_mcp_no_such_adm.wav")
check(err_code(missing) == "file_not_found", "missing path -> file_not_found",
      "error=%s" % err_code(missing))

ins = call("analysis.adm_inspect", path=ADM_OUT)
check(ins.get("isAdm") is True, "inspected file is recognized as ADM (chna + axml)")
fmt = ins.get("format", {})
check(fmt.get("channels") == 12, "adm_inspect: 12 channels", "channels=%s" % fmt.get("channels"))
chna = ins.get("chna", {})
check(isinstance(chna, dict) and chna.get("numTracks") == 12 and len(chna.get("tracks", [])) == 12,
      "chna: 12 tracks with UID/track/pack refs")
admj = ins.get("adm", {})
check(admj.get("audioObjects") == 3, "3 audioObjects (bed + 2 objects)",
      "n=%s" % admj.get("audioObjects"))
check(admj.get("hasDirectSpeakers") is True and admj.get("hasObjects") is True,
      "ADM has both a DirectSpeakers bed and Objects")
check(admj.get("audioBlockFormats", 0) >= 12, "at least 12 audioBlockFormats (10 bed + object blocks)",
      "blocks=%s" % admj.get("audioBlockFormats"))
check(len(ins.get("warnings", [])) == 0, "no structural warnings on the authored file",
      "warnings=%s" % ins.get("warnings"))

insc = call("analysis.adm_inspect", path=ADM_OUT_CART)
check(insc.get("adm", {}).get("coordinateMode") == "cartesian",
      "cartesian file reports coordinateMode=cartesian")

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    for f in (ADM_OUT, ADM_OUT_CART):
        try:
            os.remove(f)
        except OSError:
            pass
    scratch = [bed, objA, objB]
    for t in sorted({t for t in scratch if isinstance(t, int)}, reverse=True):
        call("track.remove", track=t)
    check(call("project.get_summary").get("trackCount") == base,
          "all scratch tracks removed", "count=%s base=%s"
          % (call("project.get_summary").get("trackCount"), base))
else:
    notes.append("scratch tracks + %s left in place (pass --cleanup to remove)." % ADM_OUT)

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
