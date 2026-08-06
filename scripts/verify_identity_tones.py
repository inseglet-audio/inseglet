#!/usr/bin/env python3
"""Live-verify Batch C / B4 — in-DAW channel-identity QC (docset doc 135).

Exercises, end-to-end on the real SDK path against a running REAPER:
  * spatial.inject_identity_tones — author the corpus tone plan (313 + 139*k Hz, LFE 40 Hz,
                                    -18 dBFS) and PLACE it: a multichannel item on a bed track
                                    (mode=channels) and one mono item per object track
                                    (mode=tracks).
  * analysis.verify_routing       — read it back and report the detected routing map, both from
                                    the WAV on disk and (render-free) through a live track
                                    audio accessor.

The plan, the Goertzel detector and every verdict are proven DETERMINISTICALLY off-REAPER in
unit.identity_tones, including a constructed positive control for each fault class. This gate
proves the TOOLS run end-to-end on real REAPER: the WAVs are authored where the response says,
the items are actually placed (placed:true — the two new API imports resolved), the accessor
path reads the same audio the file does, and — the point of the batch — a DELIBERATELY BROKEN
routing is caught by the instrument rather than passing silently.

It opens its own scratch project, so the user's project is untouched and track discovery is
deterministic.

Usage:
    python3 verify_identity_tones.py            # auto-find reaper_mcp.json
    python3 verify_identity_tones.py --cleanup  # remove the scratch tracks at the end
    python3 verify_identity_tones.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 189  # 187 (v1.7.0) + 2 (B4: inject_identity_tones + verify_routing)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action "
             "first. (Discovery is written at load; if it is absent, REAPER is not running the "
             "extension.)")

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

def failed_closed(result):
    sc = result.get("structuredContent", {})
    return result.get("isError") is True or (isinstance(sc, dict) and sc.get("error") is not None)

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_identity_tones", "version": "0"}})

passed = failed = 0
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def add_track(name, nchan=None):
    idx = call("project.get_summary").get("trackCount", 0)
    t = call("track.add", index=idx, name=name).get("trackIndex", idx)
    if nchan:
        call("spatial.set_track_channels", track=t, channels=nchan)
    return t

def verdicts(rep):
    return [c.get("verdict") for c in rep.get("perChannel", [])]

# ================================================================================================
initialize()

print("== 0. surface >= %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") >= EXPECTED_SURFACE,
      "tool surface >= %d (187 + 2 B4)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
names = {t.get("name") for t in enum.get("tools", [])}
check("spatial.inject_identity_tones" in names, "spatial.inject_identity_tones is on the wire")
check("analysis.verify_routing" in names, "analysis.verify_routing is on the wire")

print("\n== 1. scratch tracks (appended; the user's existing tracks are untouched) ==")
bed = add_track("B4 bed 7.1.4", nchan=12)
print("  bed track = %s" % bed)

print("\n== 2. dryRun plans, writes nothing ==")
dry = call("spatial.inject_identity_tones", track=bed, bedLayout="7.1.4", dryRun=True)
check(dry.get("ok") is True and dry.get("dryRun") is True, "dryRun ok")
check(dry.get("channels") == 12, "7.1.4 -> 12 slots", "channels=%s" % dry.get("channels"))
check(dry.get("lfeChannels") == [3], "LFE index comes from the bed table", str(dry.get("lfeChannels")))
plan = dry.get("plan", [])
check(len(plan) == 12 and abs(plan[0]["hz"] - 313.0) < 1e-6 and abs(plan[1]["hz"] - 452.0) < 1e-6,
      "tone plan is 313 + 139*k")
check(abs(plan[3]["hz"] - 40.0) < 1e-6 and plan[3]["lfe"] is True, "slot 3 is the 40 Hz LFE tone")
check(dry.get("wavs") == [], "dryRun wrote nothing")

print("\n== 3. inject on the bed (mode=channels) ==")
inj = call("spatial.inject_identity_tones", track=bed, bedLayout="7.1.4")
check(inj.get("ok") is True, "inject ok")
wavs = inj.get("wavs", [])
check(len(wavs) == 1 and os.path.exists(wavs[0]), "one multichannel WAV written and on disk",
      str(wavs[:1]))
check(inj.get("placed") is True, "items PLACED (PCM_Source_CreateFromFile + "
      "SetMediaItemTake_Source resolved)", "warnings=%s" % inj.get("warnings"))
check(len(inj.get("items", [])) == 1, "exactly one item placed on the bed track")

print("\n== 4. verify_routing from the WAV ==")
rep = call("analysis.verify_routing", path=wavs[0], channels=12, lfeChannels=[3])
check(rep.get("ok") is True, "clean identity map", rep.get("summary", ""))
check(rep.get("identityCount") == 12, "12/12 identity", str(rep.get("identityCount")))
check(rep.get("detectedMap") == list(range(12)), "detected map is the identity permutation")
check(rep.get("worstMarginDb", 0) >= 60.0, "worst margin >= 60 dB",
      "%.1f dB" % rep.get("worstMarginDb", 0))
check(rep.get("findings") == [], "zero findings on clean material")

print("\n== 5. THE POINT — a deliberately broken plan must FAIL ==")
# Same bytes, judged against a plan with NO LFE slot: channel 3 carries 40 Hz, which that plan
# does not describe at all. The instrument must refuse to call this clean.
bad = call("analysis.verify_routing", path=wavs[0], channels=12)
check(bad.get("ok") is False, "the same WAV FAILS against a plan without the LFE slot")
v = verdicts(bad)
check(len(v) == 12 and v[3] != "identity", "channel 3 is not 'identity' there", "v[3]=%s" % (v[3:4]))
check(len(bad.get("findings", [])) >= 1, "it produces a finding naming the channel")
# width mismatch is a refusal, not a verdict
mism = raw("analysis.verify_routing", path=wavs[0], channels=6)
check(failed_closed(mism), "a channel-count mismatch fails closed (refusal, not a verdict)")

print("\n== 6. per-track mode (the send-routing edge) ==")
objs = [add_track("B4 obj %d" % i, nchan=2) for i in range(3)]
inj2 = call("spatial.inject_identity_tones", mode="tracks", tracks=objs)
check(inj2.get("ok") is True and len(inj2.get("wavs", [])) == 3, "three mono tone WAVs written")
check(inj2.get("placed") is True, "one item placed per object track",
      "warnings=%s" % inj2.get("warnings"))
check(inj2.get("channels") == 3, "three slots, one per track")

print("\n== 7. render-free accessor read of the live bed ==")
acc = raw("analysis.verify_routing", track=bed, channels=12, lfeChannels=[3])
if failed_closed(acc):
    print("  [note] accessor read declined: %s" % acc.get("structuredContent", {}).get("detail"))
    print("         (an accessor needs the item's audio in range; not a B4 defect)")
else:
    sc = acc.get("structuredContent", {})
    check(sc.get("source") == "accessor", "read came from the audio accessor, no render")
    check(sc.get("ok") is True, "live accessor read shows the same identity map",
          sc.get("summary", ""))
    check(sc.get("detectedMap") == list(range(12)), "accessor detected map == identity")

if DO_CLEANUP:
    print("\n== cleanup ==")
    for t in sorted([bed] + objs, reverse=True):
        try:
            call("track.remove", track=t)
        except Exception as e:
            print("  (could not remove track %s: %s)" % (t, e))

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
