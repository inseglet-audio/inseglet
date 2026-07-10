#!/usr/bin/env python3
"""Live-verify Phase 6 batches (b) + (c) — per-bed sub-metering + the multichannel true-peak limiter.

Plain-REAPER gate (Session P6-A) — NO IEM / ambisonic suite required. Reads the endpoint + bearer token
from the extension's discovery file (reaper_mcp.json) and drives the two hardened verbs against a running
REAPER, asserting real behaviour + project state:

  surface : tool surface = 61 (unchanged — Phase 6 deepens existing tools, adds none).

  (b) analysis.check_deliverable — PER-BED sub-metering:
    b1. SUPPLIED per-bed array (deterministic, no render): 2 measurement blocks -> perBed[2] + a rollup
        whose pass is False because one bed is over target/ceiling, and whose `failing` names that bed;
    b2. dryRun with beds=[…] lists the bed buses it WOULD render and mutates nothing;
    b3. a real bounded multi-stem render of two bed buses (7.1.4 + 5.1) returns perBed[2] each carrying
        its channel width, plus a rollup — and LEAVES THE PROJECT UNCHANGED (read-only). Numeric loudness
        is best-effort (SKIP-noted if the beds are silent); the per-bed STRUCTURE is the gate;
    b4. perBed:true auto-detects the bed-width buses (>=2 found).

  (c) mix.apply_style — MULTICHANNEL limiter on a wide bed:
    c1. immersive-master-atmos on a 7.1.4 (12-ch) bed inserts the bundled multichannel JSFX limiter
        (inserted step multichannel:true; fx name contains "mcp"), ceiling sized to atmos-music (-1 dBTP);
    c2. the limiter's LFE exemption is the JSFX-internal method (LFE passed, not pin-wired) on channel 4;
    c3. NO honest-coverage warning about "only the first channels processed" for the limiter (it covers
        all mains) — any such warning here is a real regression;
    c4. a STEREO track keeps the stock 2-ch ReaLimit (multichannel:false) — the swap is wide-bed only.

If the JSFX is NOT installed, c1 FAILS and apply_style's own warning is printed telling you to run
`cmake --install build` (it ships Effects/MCP/mcp_mc_limiter.jsfx). REAPER must be in the FOREGROUND —
App Nap stalls the Run() pump. Stdlib only.

Usage:
    python3 verify_phase6bc.py                 # auto-find reaper_mcp.json
    python3 verify_phase6bc.py /path/to/reaper_mcp.json
    python3 verify_phase6bc.py --cleanup       # also delete the test tracks when done
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
    with urllib.request.urlopen(req, timeout=30) as r:
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
def fx_count(idx):
    return call("fx.list", track=idx)["fxCount"]

try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 61, "tools/list returns 61 tools (unchanged — Phase 6 adds none)",
          f"got {len(tools)}")
    check("analysis.check_deliverable" in names and "mix.apply_style" in names,
          "the two hardened verbs are present")

    # ---- b1. per-bed via SUPPLIED measurement array (deterministic, no render) --------------------
    print("\n== (b) per-bed — supplied measurement array (no render) ==")
    r = call("analysis.check_deliverable", spec="atmos-music", measured=[
        {"target": "Music Bed", "lufsIntegrated": -18.1, "truePeak": -1.3, "channels": 12},
        {"target": "FX Bed",    "lufsIntegrated": -11.5, "truePeak": -0.2, "channels": 12},
    ])
    check(isinstance(r.get("perBed"), list) and len(r["perBed"]) == 2, "perBed reports 2 beds",
          f'len={len(r.get("perBed", []))}')
    check(r["perBed"][0].get("pass") is True, "conformant bed (Music Bed) passes")
    check(r["perBed"][1].get("pass") is False, "hot bed (FX Bed) fails")
    roll = r.get("rollup", {})
    check(roll.get("pass") is False, "rollup fails because one bed fails")
    check(roll.get("failing") == ["FX Bed"], "rollup names the failing bed", str(roll.get("failing")))
    check(roll.get("worstTruePeak") == -0.2, "rollup worstTruePeak = highest measured dBTP",
          str(roll.get("worstTruePeak")))

    # ---- b2/b3/b4. render two real bed buses as stems, per-bed report -----------------------------
    print("\n== (b) per-bed — real bed buses (bounded multi-stem render) ==")
    bed714 = call("spatial.build_bed", layout="7.1.4", busName="MCP p6 Bed 714")
    t714 = bed714["busTrack"]; created.append(t714)
    bed51 = call("spatial.build_bed", layout="5.1", busName="MCP p6 Bed 51")
    t51 = bed51["busTrack"]; created.append(t51)
    check(bed714.get("channels") == 12 and bed51.get("channels") == 6,
          "built a 12-ch and a 6-ch bed bus", f'{bed714.get("channels")} + {bed51.get("channels")}')

    dr = call("analysis.check_deliverable", spec="atmos-music", beds=[t714, t51], dryRun=True)
    check(dr.get("dryRun") is True and isinstance(dr.get("beds"), list) and len(dr["beds"]) == 2,
          "dryRun lists the 2 bed buses it would render", str([b.get("channels") for b in dr.get("beds", [])]))

    n_before = track_count()
    m = call("analysis.check_deliverable", spec="atmos-music", beds=[t714, t51],
             boundsFlag=0, startPos=0.0, endPos=2.0)
    pb = m.get("perBed", [])
    check(isinstance(pb, list) and len(pb) == 2, "render produced a 2-entry per-bed report", f'len={len(pb)}')
    chans = sorted(e.get("channels") for e in pb)
    check(chans == [6, 12], "per-bed entries carry the real bed channel widths", str(chans))
    check("rollup" in m, "rollup present")
    print("  [diag] rollup   =", json.dumps(m.get("rollup", {})))
    print("  [diag] rawStats =", (m.get("rawStats") or "")[:400])
    have_lufs = any((e.get("metrics") or {}).get("lufsIntegrated") is not None for e in pb)
    if have_lufs:
        check(True, "per-bed render measured loudness via RENDER_STATS",
              f'worstTruePeak={m["rollup"].get("worstTruePeak")}')
    else:
        skip("per-bed numeric loudness", "beds silent (no routed content) — structure verified, numbers need audio")
    check(track_count() == n_before, "per-bed render left the track count unchanged (read-only)",
          f"{n_before} -> {track_count()}")

    auto = call("analysis.check_deliverable", spec="atmos-music", perBed=True,
                boundsFlag=0, startPos=0.0, endPos=1.0)
    check(len(auto.get("perBed", [])) >= 2, "perBed:true auto-detected the bed-width buses (>=2)",
          f'found {len(auto.get("perBed", []))}')

    # ---- c1/c2/c3. multichannel limiter on the 7.1.4 bed ------------------------------------------
    print("\n== (c) multichannel limiter — 7.1.4 bed (immersive-master-atmos) ==")
    apb = call("mix.apply_style", track=t714, style="immersive-master-atmos", targetSpec="atmos-music")
    check(apb.get("ok") is True, "immersive style applied to the bed")
    check(apb.get("ceilingDb") == -1.0, "atmos-music -> -1 dBTP ceiling", f'ceilingDb={apb.get("ceilingDb")}')
    for w in apb.get("warnings", []):
        print(f"  [warn] {w}")
    ins = apb.get("inserted", [])
    lim = next((s for s in ins if s.get("role") == "limiter"), None)
    check(lim is not None, "a limiter step was inserted")
    if lim is not None:
        check(lim.get("multichannel") is True,
              "the limiter is the bundled MULTICHANNEL JSFX",
              "if FAIL: run `cmake --install build` to ship Effects/MCP/mcp_mc_limiter.jsfx (see warnings)")
        check("mcp" in str(lim.get("fx", "")).lower(), "limiter fx name identifies the MCP JSFX",
              str(lim.get("fx")))
        lex = lim.get("lfeExemption", {})
        check(lex.get("method") == "jsfx-internal" and lex.get("lfeChannel") == 4,
              "LFE handled JSFX-internally on channel 4 (passed, not pin-wired)", json.dumps(lex))
    # The multichannel LIMITER must NOT get a coverage warning (it reaches all mains). A stock 2-ch
    # MULTIBAND/comp step legitimately still warns on a wide bed — that is expected and not a failure here
    # (this batch made only the limiter multichannel; a multichannel multiband is a later enhancement).
    lim_cover_warn = any(str(w).startswith("limiter '") and "only the first channels" in str(w)
                         for w in apb.get("warnings", []))
    check(not lim_cover_warn,
          "the multichannel LIMITER gets NO coverage warning (covers all mains)")
    if any("only the first channels" in str(w) for w in apb.get("warnings", [])):
        print("  [note] a stock 2-ch multiband/comp still warns on the wide bed — expected "
              "(only the limiter was made multichannel this batch).")

    # ---- c4. a stereo track keeps the stock 2-ch limiter (no MC swap) -----------------------------
    print("\n== (c) stereo target keeps the stock limiter ==")
    strk = call("track.add", name="MCP p6 Stereo")["trackIndex"]; created.append(strk)
    aps = call("mix.apply_style", track=strk, style="immersive-master-atmos")
    slim = next((s for s in aps.get("inserted", []) if s.get("role") == "limiter"), None)
    check(slim is not None and slim.get("multichannel") is not True,
          "stereo target keeps the stock 2-ch limiter (no multichannel swap)",
          str(slim.get("fx") if slim else None))

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
