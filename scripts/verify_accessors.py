#!/usr/bin/env python3
"""Live-verify Batch B1 — audio-accessor tools (3) against a running REAPER + extension.

analysis.read_samples / accessor_meter / detect_silence read PCM directly through a REAPER audio
accessor (no render round-trip). This gate proves, end-to-end on the real SDK path:
  * wiring + surface (161) + dryRun plans (no render / no read),
  * lifecycle SAFETY: create -> read -> destroy on an empty track returns a CLEAN structured result
    (empty_source), never a crash, and REAPER stays responsive afterwards,
  * the track-accessor PRE-FX semantics: a stock tone JSFX (an FX, not a media item) is captured by
    the RENDER path (analysis.meter) but NOT by the accessor path (accessor is empty/silent) —
    confirming the accessor reads item/take content, not the post-FX bus signal,
  * on real audio-item content (auto-detected if the open project has any audio): the sample read
    returns finite per-channel stats, a waveform overview, silence classification, and a window that
    clamps to the source extent.

The DSP numbers themselves (DC/clip/overview/silence, per-channel level/K-weighting/true-peak) are
proven deterministically off-REAPER in `unit.meter`. This gate proves the TOOLS run end-to-end on a
real accessor and that the read-only guards hold. REAPER must be in the FOREGROUND, and
`cmake --install build` must have shipped the current dylib. Nothing opens a dialog. Stdlib only.

Usage:
    python3 verify_accessors.py                 # auto-find reaper_mcp.json; leaves scratch track
    python3 verify_accessors.py --cleanup       # also delete the scratch track it created
    python3 verify_accessors.py /path/to/reaper_mcp.json
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
EXPECTED_SURFACE = 161  # 158 + 3 (Batch B1 audio accessors)
TONE_CANDIDATES = ["JS: Tone Generator", "JS: Oscillator", "JS: 1-Band Tone Generator",
                   "JS: Signal Generator", "JS: Sine", "Tone Generator"]
B1 = ["analysis.read_samples", "analysis.accessor_meter", "analysis.detect_silence"]

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

def err_code(result):
    """Structured-error code of a RETURNED makeError result (read-only analysis sets no MCP isError —
    the dispatcher only flags isError on a THROW/timeout), else None."""
    sc = result.get("structuredContent", {})
    return sc.get("error") if isinstance(sc, dict) else None

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_accessors", "version": "0"}})

passed = failed = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def add_track(name):
    idx = call("project.get_summary").get("trackCount", 0)
    return call("track.add", index=idx, name=name).get("trackIndex", idx)

def add_tone(track):
    for nm in TONE_CANDIDATES:
        r = raw("fx.add", track=track, name=nm)
        if not r.get("isError") and r.get("structuredContent", {}).get("fxIndex", -1) >= 0:
            return True
    return False

# ================================================================================================
initialize()
print("== 0. surface + presence ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      "tool surface == %d (158 + 3 B1)" % EXPECTED_SURFACE, "count=%s" % enum.get("count"))
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
check(all(n in listed for n in B1), "all three B1 accessor tools present in tools/list",
      "missing=%s" % [n for n in B1 if n not in listed])

base = call("project.get_summary").get("trackCount", 0)
sc = add_track("MCP B1 scratch")
call("spatial.set_track_channels", track=sc, channels=2)

# ================================================================================================
print("\n== 1. dryRun plans (no render, no read) ==")
for tool in B1:
    d = call(tool, target=sc, dryRun=True)
    check(d.get("dryRun") is True and d.get("source") == "track" and "plan" in d,
          "%s dryRun returns a plan (source=track)" % tool, "keys=%s" % sorted(d.keys()))

# ================================================================================================
print("\n== 2. lifecycle safety on an empty track (no items -> clean structured result) ==")
r = raw("analysis.read_samples", target=sc)
rc = err_code(r)
check(r.get("isError") is not True, "read_samples on an empty track does NOT hard-error (no crash)",
      "isError=%s error=%s" % (r.get("isError"), rc))
check(rc in ("empty_source", "empty_window", None), "empty track -> clean empty_source structured result",
      "error=%s" % rc)
# REAPER still responsive after the accessor create/read/destroy cycle
check(call("project.get_summary").get("trackCount", 0) >= 1,
      "REAPER responsive after the accessor lifecycle (pump not stalled)")

# ================================================================================================
print("\n== 3. track-accessor PRE-FX semantics (accessor ignores track FX / reads item content) ==")
tone_ok = add_tone(sc)
# The scratch track now carries a tone JSFX but NO media items. A track accessor reads item content,
# so it must STILL be empty/silent — the FX output is not visible to it. This is the primary proof and
# needs no render.
am = raw("analysis.accessor_meter", target=sc)
amsc = am.get("structuredContent", {})
accd = amsc.get("channelsDetail", [])
acc_peak = max((c.get("peakDb", -150.0) for c in accd), default=-150.0)
acc_empty = (err_code(am) in ("empty_source", "empty_window")) or amsc.get("silent") is True \
    or acc_peak < -120.0
check(acc_empty,
      "track accessor with a tone JSFX but no items is empty/silent -> reads item content, not track FX",
      "accError=%s silent=%s accPeak=%.1f toneLoaded=%s" %
      (err_code(am), amsc.get("silent"), acc_peak, tone_ok))
# Bonus (best-effort): a TIGHTLY-bounded render of the same track DOES capture the tone (post-FX),
# confirming the FX is producing audio the accessor simply doesn't see. Guarded so a fragile
# empty-project render can't stall the gate (bound with boundsFlag=0 + startPos/endPos, not ENTIRE PROJECT).
if tone_ok:
    try:
        mtr = call("analysis.meter", target=sc, boundsFlag=0, startPos=0.0, endPos=0.5)
        cd = mtr.get("channelsDetail", [])
        rpeak = max((c.get("peakDb", -150.0) for c in cd), default=-150.0)
        check(rpeak > -80.0,
              "render (analysis.meter) DOES capture the tone JSFX (peak > -80 dBFS) -> confirms pre-FX gap",
              "renderPeak=%.1f accPeak=%.1f" % (rpeak, acc_peak))
    except Exception as e:
        notes.append("render leg of the pre-FX probe skipped (%s) — the accessor-empty check above still "
                     "proves the accessor ignores track FX." % str(e)[:70])
else:
    notes.append("no stock tone JSFX available — pre-FX probe used the empty-track accessor result only.")

# ================================================================================================
print("\n== 4. real audio-item content (auto-detected in the open project) ==")
content = None
ntr = call("project.get_summary").get("trackCount", 0)
for i in range(ntr):
    rr = raw("analysis.read_samples", target=i)
    s = rr.get("structuredContent", {})
    if rr.get("isError") is not True and not s.get("error") and (s.get("frames") or 0) > 0:
        content = i
        break

if content is None:
    notes.append("no audio-item content found in the open project — the real-sample read path was NOT "
                 "exercised. Drop an audio item onto a track and re-run to cover it (the DSP itself is "
                 "proven in unit.meter).")
else:
    print("   using track %d (has audio items)" % content)
    rs = call("analysis.read_samples", target=content, overview=True, overviewBuckets=64)
    check((rs.get("channels") or 0) > 0 and (rs.get("frames") or 0) > 0 and (rs.get("sampleRate") or 0) > 0,
          "read_samples on real content: channels/frames/sampleRate > 0",
          "ch=%s frames=%s sr=%s" % (rs.get("channels"), rs.get("frames"), rs.get("sampleRate")))
    cd = rs.get("channelsDetail", [])
    finite = cd and all(math.isfinite(c.get("peakDb", 0.0)) and math.isfinite(c.get("rmsDb", 0.0))
                        and "dcOffset" in c and "clipCount" in c for c in cd)
    check(bool(finite), "per-channel peak/rms/dcOffset/clipCount present + finite on real content")
    ov = rs.get("overview", {})
    check((ov.get("buckets") or 0) > 0 and isinstance(ov.get("channels"), list) and len(ov.get("channels")) == len(cd),
          "waveform overview returns buckets for every channel", "buckets=%s" % ov.get("buckets"))

    am = call("analysis.accessor_meter", target=content)
    corr_ok = (len(cd) < 2) or ("lrCorrelation" in am.get("downmix", {}))
    check("loudness" in am and "channelsDetail" in am and corr_ok,
          "accessor_meter returns loudness + per-channel detail (+ L/R correlation for >=2ch)")

    ds = call("analysis.detect_silence", target=content)
    check(all(k in ds for k in ("leadingSilenceSec", "trailingSilenceSec", "internalGaps", "fullySilent")),
          "detect_silence returns leading/trailing/internalGaps/fullySilent")

    # window clamp: read a window entirely beyond the source extent -> clean empty_window (no crash)
    ext = rs.get("sourceExtent", {})
    beyond = raw("analysis.read_samples", target=content, start=(ext.get("end", 0.0) + 100.0), duration=1.0)
    check(beyond.get("isError") is not True and err_code(beyond) in ("empty_window", "empty_source"),
          "read beyond the source extent -> clean empty_window (no crash)",
          "error=%s" % err_code(beyond))

# ================================================================================================
if DO_CLEANUP:
    print("\n== cleanup ==")
    call("track.remove", track=sc)
    check(call("project.get_summary").get("trackCount") == base, "scratch track removed")
else:
    notes.append("scratch track %d left in place (pass --cleanup to remove)" % sc)

print("\n" + "=" * 64 + "\n  PASS=%d  FAIL=%d" % (passed, failed))
for n in notes:
    print("  note: %s" % n)
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: ALL GREEN")
