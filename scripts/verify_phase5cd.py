#!/usr/bin/env python3
"""Live-verify Phase 5 batch (c)+(d) — deliverable conformance + immersive-aware style chains.

Plain-REAPER gate (Session B) — NO IEM / ambisonic suite required. Reads the endpoint + bearer token
from the extension's discovery file (reaper_mcp.json), then drives the two new verbs against a running
REAPER and asserts real behaviour + project state:

  1. tool surface = 60, and analysis.check_deliverable + mix.apply_style are present;
  2. (c) check_deliverable JUDGEMENT via SUPPLIED measurements (no render — deterministic): ebu-r128,
     atsc-a85, cinema-theatrical (SPL: LUFS not gated, TP gates), ebu-r128-s1 (Max-ST gate), podcast
     mono/stereo target, tolerance override, and a structured unknown_spec error;
  3. (c) check_deliverable RENDER MECHANICS: dryRun returns a plan + mutates nothing; a bounded, real
     analysis render (best-effort tone source) returns numeric LUFS + true-peak from RENDER_STATS and
     LEAVES THE PROJECT UNCHANGED (read-only). Prints the raw truePeak so you can confirm it is dBTP and
     cross-check with `ffmpeg -i <file> -af ebur128 -f null -` (the one assumption flagged for this gate);
  4. (d) apply_style on a STEREO track: inserts the pop-bright chain (in-box ReaEQ/ReaComp/ReaLimit),
     fx.list confirms the FX landed, a limiter is present, the ceiling = the spec true-peak; dryRun adds
     nothing; a re-run is IDEMPOTENT (reused, FX count unchanged);
  5. (d) apply_style on a 7.1.4 BED: immersive-master-atmos is immersive-aware — ceiling sized to the
     target spec's TP, hasLfe true, the LFE channel reported, each dynamics step carries an lfeExemption
     report, and targetSpec overrides the ceiling (-1 atmos-music vs -2 atsc-a85);
  6. (d) a structured unknown_style error.

If no stock tone generator can be loaded, the live-render leg (3) reports SKIPPED — the judgement (2) and
all of (d) still run. If the in-box limiter name ("ReaLimit") is wrong on this install, (4)'s "limiter
present" check FAILS and prints apply_style's own warning so you know exactly which stock name to fix in
src/style_palette.h. REAPER must be in the FOREGROUND — App Nap stalls the Run() pump. Stdlib only.

Usage:
    python3 verify_phase5cd.py                 # auto-find reaper_mcp.json
    python3 verify_phase5cd.py /path/to/reaper_mcp.json
    python3 verify_phase5cd.py --cleanup       # also delete the test tracks when done
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),  # macOS
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),                      # Linux
]

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action first.")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
print(f"endpoint : {URL}\ntoken    : {TOKEN[:8]}…\n")

_id = 0
def rpc(method, params=None):
    global _id
    _id += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Content-Type": "application/json", "Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=20) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    """Return structuredContent. A recoverable {error,...} result is NOT an isError — it is returned as
    data (the agent-actionable structured-error channel), so callers inspect result.get('error')."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r["structuredContent"]

passed = failed = skipped = 0
def check(cond, label, detail=""):
    global passed, failed
    mark = "PASS" if cond else "FAIL"
    if cond: passed += 1
    else: failed += 1
    print(f"  [{mark}] {label}" + (f"  — {detail}" if detail else ""))

def skip(label, detail=""):
    global skipped
    skipped += 1
    print(f"  [SKIP] {label}" + (f"  — {detail}" if detail else ""))

def track_count():
    return len(call("track.list")["tracks"])

created = []
def add_src(name, channels=None):
    idx = call("track.add", name=name)["trackIndex"]
    created.append(idx)
    if channels:
        call("spatial.set_track_channels", track=idx, channels=channels)
    return idx

def fx_count(idx):
    return call("fx.list", track=idx)["fxCount"]

try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 60, "tools/list returns 60 tools (58 + check_deliverable + apply_style)",
          f"got {len(tools)}")
    check("analysis.check_deliverable" in names, "analysis.check_deliverable present")
    check("mix.apply_style" in names, "mix.apply_style present")

    # ---- 2. (c) check_deliverable — judgement via supplied measurements (no render) ---------------
    print("\n== (c) check_deliverable — supplied-measurement judgement ==")
    r = call("analysis.check_deliverable", spec="ebu-r128",
             measured={"lufsIntegrated": -23.0, "truePeak": -1.5})
    check(r.get("pass") is True, "ebu-r128 -23.0 / -1.5 dBTP -> pass", str(r.get("pass")))
    check(r.get("measuredSource") == "supplied", "evaluated the supplied numbers (no render)")

    r = call("analysis.check_deliverable", spec="ebu-r128",
             measured={"lufsIntegrated": -22.0, "truePeak": -1.5})
    check(r.get("pass") is False, "ebu-r128 -22.0 (+1.0 LU) -> fail (outside ±0.5)")

    r = call("analysis.check_deliverable", spec="atsc-a85",
             measured={"lufsIntegrated": -21.0, "truePeak": -0.5})
    check(r.get("pass") is False, "atsc-a85 -21.0 / -0.5 -> fail (too loud + over -2 dBTP)")

    r = call("analysis.check_deliverable", spec="cinema-theatrical",
             measured={"lufsIntegrated": -30.0, "truePeak": -3.4})
    check(r.get("pass") is True, "cinema-theatrical passes on TP alone (LUFS not gated)")
    r = call("analysis.check_deliverable", spec="cinema-theatrical",
             measured={"lufsIntegrated": -30.0, "truePeak": -2.5})
    check(r.get("pass") is False, "cinema-theatrical TP -2.5 over -3 -> fail")

    r = call("analysis.check_deliverable", spec="ebu-r128-s1",
             measured={"lufsIntegrated": -23.0, "truePeak": -1.2, "shortTermMax": -17.0})
    check(r.get("pass") is False, "ebu-r128-s1 Max-ST -17 exceeds -18 -> fail")
    r = call("analysis.check_deliverable", spec="ebu-r128-s1",
             measured={"lufsIntegrated": -23.0, "truePeak": -1.2, "shortTermMax": -19.0})
    check(r.get("pass") is True, "ebu-r128-s1 Max-ST -19 under -18 -> pass")

    r = call("analysis.check_deliverable", spec="podcast",
             measured={"lufsIntegrated": -19.0, "truePeak": -1.5, "channels": 1})
    check(r.get("pass") is True, "podcast -19 mono -> pass (mono target)")
    r = call("analysis.check_deliverable", spec="podcast",
             measured={"lufsIntegrated": -19.0, "truePeak": -1.5, "channels": 2})
    check(r.get("pass") is False, "podcast -19 in stereo -> fail (-16 target)")

    r = call("analysis.check_deliverable", spec="ebu-r128", toleranceLU=1.5,
             measured={"lufsIntegrated": -22.0, "truePeak": -1.5})
    check(r.get("pass") is True, "toleranceLU=1.5 override lets -22.0 pass")

    r = call("analysis.check_deliverable", spec="banana",
             measured={"lufsIntegrated": -23.0, "truePeak": -1.0})
    check(r.get("error") == "unknown_spec", "unknown spec -> structured unknown_spec error",
          str(r.get("error")))
    check("remediation" in r, "unknown_spec carries a remediation (known specs)")

    # ---- 3. (c) check_deliverable — render mechanics (dryRun + a real bounded render) -------------
    print("\n== (c) check_deliverable — render mechanics (non-destructive) ==")
    dr = call("analysis.check_deliverable", spec="ebu-r128", dryRun=True)
    check(dr.get("dryRun") is True, "dryRun returns a measurement plan", str(dr.get("plan"))[:60])

    # Best-effort audible source so a bounded render has signal. Skip the render leg if none loads.
    gen = add_src("MCP p5cd Tone")
    gen_fx = -1
    for cand in ("Tone Generator", "signal_generator", "JS: Tone Generator", "ReaSynth"):
        try:
            res = call("fx.add", track=gen, name=cand)
            if res.get("fxIndex", -1) >= 0:
                gen_fx = res["fxIndex"]
                print(f"  [info] tone source = '{res.get('name', cand)}'")
                break
        except Exception:
            pass
    if gen_fx < 0:
        skip("live analysis render", "no stock tone generator loaded — open a project with audio to test the render leg")
    else:
        n_before = track_count()
        m = call("analysis.check_deliverable", spec="atsc-a85", target="master",
                 boundsFlag=0, startPos=0.0, endPos=2.0)
        # DIAGNOSTIC: dump the exact RENDER_STATS tokens + parsed metrics so we can see whether/how
        # REAPER reports true peak in this normalization config.
        print("  [diag] metrics   =", json.dumps(m.get("metrics", {})))
        print("  [diag] rawStats  =", (m.get("rawStats") or "")[:500])
        if m.get("measuredSource") == "render-stats" and "metrics" in m and m["metrics"].get("lufsIntegrated") is not None:
            met = m["metrics"]
            check(True, "bounded master render measured via RENDER_STATS",
                  f'LUFS-I={met.get("lufsIntegrated")}  truePeak={met.get("truePeak")}')
            check(met.get("truePeak") is not None, "true-peak token present in RENDER_STATS")
            print(f"  [!!] CONFIRM UNITS: raw truePeak = {met.get('truePeak')} — expected dBTP. "
                  f"Cross-check the analysis WAV with:  ffmpeg -i <file> -af ebur128 -f null -")
        else:
            skip("live analysis render measurement",
                 f'no loudness tokens (silent render?) — measuredSource={m.get("measuredSource")}')
        check(track_count() == n_before, "analysis render left the track count unchanged (read-only)",
              f"{n_before} -> {track_count()}")

    # ---- 4. (d) apply_style on a STEREO track ----------------------------------------------------
    print("\n== (d) apply_style — stereo track (pop-bright) ==")
    strk = add_src("MCP p5cd Stereo")
    n0 = fx_count(strk)
    dr = call("mix.apply_style", track=strk, style="pop-bright", dryRun=True)
    check(isinstance(dr.get("steps"), list) and len(dr["steps"]) >= 3, "dryRun resolves a >=3-step chain",
          f'steps={len(dr.get("steps", []))}')
    check(fx_count(strk) == n0, "dryRun inserted NO FX", f"{n0} -> {fx_count(strk)}")

    ap = call("mix.apply_style", track=strk, style="pop-bright")
    check(ap.get("ok") is True, "apply_style ok", ap.get("style"))
    ins = ap.get("inserted", [])
    check(len(ins) >= 1, "at least one FX inserted", f"{len(ins)} inserted")
    warns = ap.get("warnings", [])
    if warns:
        for w in warns:
            print(f"  [warn] {w}")
    got_limiter = any(s.get("role") == "limiter" or "limit" in str(s.get("fx", "")).lower() for s in ins)
    check(got_limiter, "a limiter FX was inserted (in-box ReaLimit resolves)",
          "if FAIL, adjust the in-box limiter name in style_palette.h (see warnings above)")
    fx_names = [f["name"] for f in call("fx.list", track=strk)["fx"]]
    check(fx_count(strk) >= n0 + len(ins), "the chain FX really landed on the track", str(fx_names))
    check(isinstance(ap.get("ceilingDb"), (int, float)), "limiter ceiling reported",
          f'ceilingDb={ap.get("ceilingDb")}')

    # idempotent re-run
    n_after = fx_count(strk)
    ap2 = call("mix.apply_style", track=strk, style="pop-bright")
    check(ap2.get("reused") is True, "re-applying the same style is idempotent (reused)", str(ap2.get("reused")))
    check(fx_count(strk) == n_after, "idempotent re-run did NOT stack a second chain",
          f"{n_after} -> {fx_count(strk)}")

    # ---- 5. (d) apply_style on a 7.1.4 BED (immersive-aware) --------------------------------------
    print("\n== (d) apply_style — 7.1.4 bed (immersive-master-atmos) ==")
    bed = call("spatial.build_bed", layout="7.1.4", busName="MCP p5cd Bed")
    bedTrk = bed["busTrack"]
    created.append(bedTrk)
    check(bed.get("channels") == 12, "bed bus built at 12ch", f'channels={bed.get("channels")}')

    apb = call("mix.apply_style", track=bedTrk, style="immersive-master-atmos", targetSpec="atmos-music")
    check(apb.get("ok") is True, "immersive style applied to the bed")
    check(apb.get("immersiveAware") is True, "reports immersiveAware")
    check(apb.get("ceilingDb") == -1.0, "atmos-music targetSpec -> -1 dBTP ceiling", f'ceilingDb={apb.get("ceilingDb")}')
    check(apb.get("hasLfe") is True and apb.get("lfeChannel") == 4, "LFE detected on the bed (channel 4)",
          f'hasLfe={apb.get("hasLfe")} lfeChannel={apb.get("lfeChannel")}')
    exempt_steps = [s for s in apb.get("inserted", []) if "lfeExemption" in s]
    check(len(exempt_steps) >= 1, "dynamics steps carry an LFE-exemption report",
          f'{[s.get("role") for s in exempt_steps]}')
    if apb.get("warnings"):
        for w in apb["warnings"]:
            print(f"  [warn] {w}")

    apb2 = call("mix.apply_style", track=bedTrk, style="immersive-master-atmos", targetSpec="atsc-a85", dryRun=True)
    check(apb2.get("ceilingDb") == -2.0, "targetSpec=atsc-a85 overrides the ceiling to -2 dBTP",
          f'ceilingDb={apb2.get("ceilingDb")}')

    # ---- 6. (d) structured error: unknown style --------------------------------------------------
    print("\n== (d) structured errors ==")
    ue = call("mix.apply_style", track=strk, style="banana")
    check(ue.get("error") == "unknown_style", "unknown style -> structured unknown_style error",
          str(ue.get("error")))
    check("remediation" in ue, "unknown_style lists the known styles as remediation")

finally:
    if CLEANUP and created:
        for idx in sorted({i for i in created if i is not None}, reverse=True):
            try:
                call("track.remove", track=idx)
            except Exception as e:
                print(f"(cleanup: could not remove track {idx}: {e})")
        print(f"\n(cleaned up {len({i for i in created if i is not None})} test tracks)")

print(f"\n{'='*52}\n{passed} passed, {failed} failed, {skipped} skipped")
if not CLEANUP and created:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
