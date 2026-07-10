#!/usr/bin/env python3
"""Live-verify Phase 5 batch (b) — outcome-level spatial composite verbs.

Reads the endpoint + bearer token from the extension's discovery file (reaper_mcp.json), then drives
the three new verbs against a running REAPER and asserts real project state:

  1. tool surface = 58, and the 3 verbs are present;
  2. spatialize_stems into a 7.1.4 BED (drums wide, vocal front-center, sub -> lfe): the bed bus is
     built at 12ch, each non-LFE stem gets a ReaSurroundPan + a send, the LFE stem is routed by a
     mono send, a monitor bus appears, and the source tracks still exist;
  3. dryRun mutates NOTHING (track count unchanged, plan returned);
  4. re-running is IDEMPOTENT (an existing bed of the same name is reused, not duplicated);
  5. spatialize_stems into a 3rd-order ambisonic SCENE (front-center / side / overhead): the scene bus
     is 16ch and each source's encoder is pointed at the reference angle — az 0 / az 105 / el 45 — which
     is read back off the live plug-in param (proves placement_defaults -> encoder end-to-end);
  6. stereo_to_ambisonic: a 1st-order sketch = 4ch + encoder, order/normalization reported, sketch flag;
  7. setup_immersive_session: DESTRUCTIVE-gate returns confirmation_required on a non-empty project,
     then with confirm:true builds bed + N object tracks + monitor + the Dolby renderer send layout;
  8. structured errors: an unknown placement descriptor is an agent-actionable {error,...} result.

The ambisonic assertions need an encoder (IEM StereoEncoder / SPARTA / ATK); without one those blocks
report SKIPPED (as batch c/e do). REAPER must be in the FOREGROUND — App Nap stalls the Run() pump that
applies FX param writes. Stdlib only (urllib).

Usage:
    python3 verify_phase5b.py                 # auto-find reaper_mcp.json
    python3 verify_phase5b.py /path/to/reaper_mcp.json
    python3 verify_phase5b.py --cleanup       # also delete the test tracks when done
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
    with urllib.request.urlopen(req, timeout=10) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    """Return structuredContent. NOTE: a recoverable {error,...} result is NOT an isError — it is
    returned as data (that's the agent-actionable structured-error channel), so callers inspect
    result.get('error') for those cases."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r["structuredContent"]

passed = failed = 0
def check(cond, label, detail=""):
    global passed, failed
    mark = "PASS" if cond else "FAIL"
    if cond: passed += 1
    else: failed += 1
    print(f"  [{mark}] {label}" + (f"  — {detail}" if detail else ""))

def track_count():
    return len(call("track.list")["tracks"])

def tracks_named(name):
    return [t for t in call("track.list")["tracks"] if t.get("name") == name]

created = []
def add_src(name):
    idx = call("track.add", name=name)["trackIndex"]
    created.append(idx)
    return idx

try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 58, "tools/list returns 58 tools (55 + 3 composite verbs)", f"got {len(tools)}")
    for v in ("spatial.spatialize_stems", "spatial.stereo_to_ambisonic", "spatial.setup_immersive_session"):
        check(v in names, f"{v} present")

    det = call("spatial.detect_spatial_suites")
    pref = det.get("preferred", {})
    have_encoder = pref.get("encoder") is not None
    print(f"  [info] preferred encoder = {(pref.get('encoder') or {}).get('name')}")

    # ---- 2. spatialize_stems -> 7.1.4 bed --------------------------------------------------------
    print("\n== (b) spatialize_stems -> 7.1.4 bed ==")
    drums = add_src("MCP p5b Drums")
    vocal = add_src("MCP p5b Vocal")
    sub = add_src("MCP p5b Sub")

    before = track_count()
    sp = call("spatial.spatialize_stems",
              sources=[drums, vocal, sub],
              target={"layout": "7.1.4"},
              placements=["wide", "front-center", "lfe"],
              monitor=True)
    check(sp.get("ok") is True, "spatialize_stems ok", sp.get("target"))
    check("7.1.4 bed" in sp.get("target", ""), "target reports a 7.1.4 bed", sp.get("target"))
    bedTrk = sp.get("bus", -1)
    bt = next((t for t in call("track.list")["tracks"] if t["index"] == bedTrk), None)
    check(bt is not None and bt["channels"] == 12, "bed bus really 12ch in REAPER",
          f'channels={bt["channels"] if bt else "?"}')
    pls = sp.get("placements", [])
    check(len(pls) == 3, "one placement report per source", f"got {len(pls)}")
    lfe_rep = next((p for p in pls if p.get("lfe")), None)
    check(lfe_rep is not None and lfe_rep.get("sendIndex", -1) >= 0,
          "the LFE stem is routed by a send (no panner)", str(lfe_rep))
    nonlfe = [p for p in pls if not p.get("lfe")]
    check(all(p.get("sendIndex", -1) >= 0 for p in nonlfe),
          "each non-LFE stem got a send into the bed")
    check(sp.get("monitor") is not None, "a monitor bus was created", str((sp.get("monitor") or {}).get("mode")))
    if sp.get("monitor"):
        created.append(sp["monitor"]["monitorTrack"])
    # source tracks untouched (still present)
    live_names = {t.get("name") for t in call("track.list")["tracks"]}
    check({"MCP p5b Drums", "MCP p5b Vocal", "MCP p5b Sub"} <= live_names, "source stems still exist")

    # ---- 3. dryRun mutates nothing ---------------------------------------------------------------
    print("\n== (b) dryRun mutates nothing ==")
    n0 = track_count()
    dr = call("spatial.spatialize_stems", sources=[drums], target={"layout": "5.1"},
              placements=["front-center"], dryRun=True)
    check(dr.get("dryRun") is True, "dryRun flagged", str(dr.get("stepCount")))
    check(isinstance(dr.get("plan"), list) and len(dr["plan"]) > 0, "a plan was returned")
    check(track_count() == n0, "no tracks were created by a dryRun", f"{n0} -> {track_count()}")

    # ---- 4. idempotent re-run --------------------------------------------------------------------
    print("\n== (b) idempotent re-run reuses the bed ==")
    sp2 = call("spatial.spatialize_stems", sources=[drums, vocal, sub], target={"layout": "7.1.4"},
               placements=["wide", "front-center", "lfe"])
    check(sp2.get("reusedBus") is True, "an existing bed of the same name is reused, not rebuilt")
    check(len(tracks_named("7.1.4 Bed")) == 1, "there is exactly one '7.1.4 Bed' track (not duplicated)",
          f'count={len(tracks_named("7.1.4 Bed"))}')

    # ---- 5. spatialize_stems -> 3rd-order ambisonic scene (reference angles, read back live) -----
    print("\n== (b) spatialize_stems -> 3rd-order ambisonic scene (reference geometry, live) ==")
    if not have_encoder:
        print("  [info] no ambisonic encoder installed — install the IEM Plug-in Suite to fully close")
        print("         batch b. Verifying a clean structured 'no_ambisonic_suite' result instead:")
        s0 = add_src("MCP p5b Amb")
        r = call("spatial.spatialize_stems", sources=[s0], target={"ambisonicOrder": 3},
                 placements=["overhead"])
        check(r.get("error") == "no_ambisonic_suite",
              "no-suite spatialize returns a structured no_ambisonic_suite error", str(r.get("error")))
        check("remediation" in r, "the structured error carries a remediation")
    else:
        a1 = add_src("MCP p5b Amb FC")
        a2 = add_src("MCP p5b Amb Side")
        a3 = add_src("MCP p5b Amb Over")
        spa = call("spatial.spatialize_stems", sources=[a1, a2, a3],
                   target={"ambisonicOrder": 3},
                   placements=["front-center", "side", "overhead"])
        check(spa.get("ok") is True, "ambisonic spatialize ok", spa.get("target"))
        sceneTrk = spa.get("bus", -1)
        st = next((t for t in call("track.list")["tracks"] if t["index"] == sceneTrk), None)
        check(st is not None and st["channels"] == 16, "scene bus is 16ch (3rd-order HOA)",
              f'channels={st["channels"] if st else "?"}')
        aps = spa.get("placements", [])
        check(len(aps) == 3 and all(p.get("fxIndex", -1) >= 0 for p in aps),
              "each source got an ambisonic encoder", f"{[p.get('encoder') for p in aps]}")

        def req_deg(prep, axis):
            for pos in prep.get("positions", []):
                if pos.get("axis") == axis:
                    return pos.get("requestedDeg")
            return None

        # front-center -> az 0 ; side -> az +105 ; overhead -> el +45  (the reference numbers)
        check(abs((req_deg(aps[0], "azimuth") or 0.0) - 0.0) < 0.5, "front-center encoder az = 0°",
              f'az={req_deg(aps[0], "azimuth")}')
        check(abs((req_deg(aps[1], "azimuth") or 0.0) - 105.0) < 0.5, "side encoder az = +105°",
              f'az={req_deg(aps[1], "azimuth")}')
        check(abs((req_deg(aps[2], "elevation") or 0.0) - 45.0) < 0.5, "overhead encoder el = +45°",
              f'el={req_deg(aps[2], "elevation")}')

        # Read the SIDE source's azimuth param back off the live plug-in and confirm it equals the
        # value the verb wrote (proves the degrees->normalized write reached the plug-in).
        side = aps[1]
        azpos = next((p for p in side.get("positions", []) if p.get("axis") == "azimuth"), None)
        if azpos and azpos.get("param") is not None:
            live = call("fx.get_param", track=a2, fx=side["fxIndex"], param=azpos["param"])
            check(abs(live.get("value", -999) - azpos.get("value", 999)) < 1e-3,
                  "the side encoder's azimuth param holds the written value in the live plug-in",
                  f'live={round(live.get("value", 0), 4)} wrote={round(azpos.get("value", 0), 4)}')

    # ---- 6. stereo_to_ambisonic ------------------------------------------------------------------
    print("\n== (b) stereo_to_ambisonic (1st-order sketch) ==")
    if not have_encoder:
        print("  [info] skipped — no encoder installed.")
    else:
        mix = add_src("MCP p5b Mix")
        sa = call("spatial.stereo_to_ambisonic", source=mix, order=1)
        check(sa.get("ok") is True and sa.get("sketch") is True, "stereo_to_ambisonic ok + labelled a sketch")
        check(sa.get("channels") == 4, "1st-order sketch = 4 channels", f'channels={sa.get("channels")}')
        mt = next((t for t in call("track.list")["tracks"] if t["index"] == mix), None)
        check(mt is not None and mt["channels"] == 4, "source track set to 4ch in REAPER")
        check(sa.get("encoder"), "an encoder was inserted", sa.get("encoder"))
        check("order" in str(sa.get("orderParam")) or sa.get("orderParam") is not None,
              "the encoder's Order param is reported (not blind-set)")

    # ---- 7. setup_immersive_session (destructive gate + full build) ------------------------------
    print("\n== (b) setup_immersive_session (confirm gate) ==")
    gate = call("spatial.setup_immersive_session", bed="7.1.4", objectCount=4, monitor=True, rendererSends=True)
    check(gate.get("error") == "confirmation_required",
          "destructive setup on a non-empty project requires confirm:true", str(gate.get("error")))
    ss = call("spatial.setup_immersive_session", bed="7.1.4", objectCount=4, monitor=True,
              rendererSends=True, confirm=True)
    check(ss.get("ok") is True, "setup_immersive_session builds with confirm:true")
    if ss.get("ok"):
        created.append(ss.get("bedBus"))
        for oi in ss.get("objectTracks", []):
            created.append(oi)
        if ss.get("monitor"):
            created.append(ss["monitor"].get("monitorTrack"))
        if ss.get("rendererLayout") and ss["rendererLayout"].get("rendererTrack", -1) >= 0:
            created.append(ss["rendererLayout"]["rendererTrack"])
        bb = next((t for t in call("track.list")["tracks"] if t["index"] == ss["bedBus"]), None)
        check(bb is not None and bb["channels"] == 12, "session bed bus is 12ch (7.1.4)")
        check(len(ss.get("objectTracks", [])) == 4, "4 object tracks created",
              f'got {len(ss.get("objectTracks", []))}')
        check(ss.get("rendererLayout") is not None, "a Dolby renderer send layout was produced",
              str((ss.get("rendererLayout") or {}).get("objectCount")))

    # ---- 8. structured error: unknown descriptor -------------------------------------------------
    print("\n== (b) structured errors ==")
    ue = call("spatial.spatialize_stems", sources=[drums], target={"layout": "7.1.4"},
              placements=["banana"])
    check(ue.get("error") == "unknown_descriptor",
          "an unknown placement descriptor is a structured, agent-actionable result", str(ue.get("error")))
    check("remediation" in ue, "the structured error lists the known descriptors as remediation")

finally:
    if CLEANUP and created:
        for idx in sorted({i for i in created if i is not None}, reverse=True):
            try:
                call("track.remove", track=idx)
            except Exception as e:
                print(f"(cleanup: could not remove track {idx}: {e})")
        print(f"\n(cleaned up {len({i for i in created if i is not None})} test tracks)")

print(f"\n{'='*52}\n{passed} passed, {failed} failed")
if not CLEANUP and created:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
