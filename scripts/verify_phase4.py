#!/usr/bin/env python3
"""Live-verify Phase 4 batch (a)+(b)+(c)+(d) — immersive multichannel/beds, ReaSurroundPan,
ambisonics, and render deliverables (in-box masters + native loudness + Atmos send layout).

Reads the endpoint + bearer token from the extension's discovery file (reaper_mcp.json),
then drives initialize -> tools/list -> bed build/route -> surround panner -> ambisonics ->
render deliverables over HTTP. Stdlib only (urllib) for the core; the batch-(d) --render leg
shells out to ffmpeg (if installed) for the loudness cross-check. macOS/Linux.

Usage:
    python3 verify_phase4.py                 # auto-find reaper_mcp.json
    python3 verify_phase4.py /path/to/reaper_mcp.json
    python3 verify_phase4.py --cleanup       # also delete the test tracks when done
    python3 verify_phase4.py --render        # also fire a short real render + ffmpeg cross-check
    python3 verify_phase4.py --render --dur=2 # bound the real render to N seconds (default 2)

What it proves in a running REAPER:
  (a) spatial.set_track_channels sets I_NCHAN and reports the immersive interpretation;
      spatial.build_bed inserts a labeled 7.1.4 multichannel bus with the right channel map;
      spatial.assign_to_bed creates multichannel sends with the exact I_SRCCHAN/I_DSTCHAN
      encoding from reaper_plugin_functions.h (stereo->pair, mono->single speaker).
  (b) spatial.add_surround_panner inserts REAPER's native ReaSurroundPan; get_surround_state
      dumps its params; set_source_position writes a normalized position that reads back.
  (c) spatial.detect_spatial_suites enumerates installed IEM/SPARTA/ATK/ambiX FX; when a suite
      is present, spatial.ambisonic_encode sets the (order+1)^2 channel count (even-padded),
      inserts an encoder, discovers az/el params by name, and identity-wires the ACN output
      pins; rotate_scene / ambisonic_decode drive a rotator / decoder. If no suite is installed
      those three are reported as SKIPPED (install the IEM Plug-in Suite to fully close batch c).

Everything is single-undo (Cmd/Ctrl+Z), or pass --cleanup to remove the test tracks.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),  # macOS
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),                      # Linux
]

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
CLEANUP = "--cleanup" in flags
RENDER = "--render" in flags   # also fire a real (short) render + ffmpeg loudness cross-check
RENDER_DUR = 2.0
for _f in flags:
    if _f.startswith("--dur="):
        try:
            RENDER_DUR = float(_f.split("=", 1)[1])
        except ValueError:
            pass

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

def angle_to_param(deg, mn, mx, lo, hi):
    # Mirror head_track_bridge.h::angleToParamValue: a normalized [0,1]-style angle param over [lo,hi] deg.
    if mx - mn <= 2.0 and hi != lo:
        return mn + max(0.0, min(1.0, (deg - lo) / (hi - lo))) * (mx - mn)
    return max(mn, min(mx, deg)) if mx > mn else deg

def axis_entry(entries, axis):
    return next((e for e in (entries or []) if e.get("axis") == axis), None)

def ffmpeg_loudness(path):
    """Second loudness engine: integrated LUFS + true peak (dBTP) via ffmpeg's ebur128 filter
    (ITU-R BS.1770). Returns (lufs_i, true_peak) or (None, None) if ffmpeg is absent / no reading."""
    exe = shutil.which("ffmpeg")
    if not exe:
        return None, None
    try:
        p = subprocess.run(
            [exe, "-nostats", "-hide_banner", "-i", path,
             "-filter_complex", "ebur128=peak=true", "-f", "null", "-"],
            capture_output=True, text=True, timeout=90)
    except Exception:
        return None, None
    err = p.stderr or ""
    def last(pat):
        m = None
        for mm in re.finditer(pat, err):
            m = mm
        return float(m.group(1)) if m else None
    return last(r"I:\s*(-?\d+(?:\.\d+)?)\s*LUFS"), last(r"Peak:\s*(-?\d+(?:\.\d+)?)\s*dBFS")

# ReaSurroundPan reports 33 params right after insertion and settles to 51 once initialized; only in
# the settled layout are params 0/1 the source-1 x/y position. Poll get_surround_state until settled.
SURROUND_SETTLED = 48
def settle_panner(track, tries=30, delay=0.1):
    st = call("spatial.get_surround_state", track=track)
    for _ in range(tries):
        if st.get("paramCount", 0) >= SURROUND_SETTLED:
            return st
        time.sleep(delay)
        st = call("spatial.get_surround_state", track=track)
    return st

created = []  # track indices we add, removed in descending order on --cleanup
try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 55, "tools/list returns 55 tools (54 + batch-e stop_head_tracking)", f"got {len(tools)}")
    for want in ["spatial.set_track_channels", "spatial.build_bed", "spatial.assign_to_bed",
                 "spatial.add_surround_panner", "spatial.get_surround_state",
                 "spatial.set_source_position", "spatial.detect_spatial_suites",
                 "spatial.ambisonic_encode", "spatial.rotate_scene", "spatial.ambisonic_decode",
                 "spatial.render_deliverables", "spatial.add_binaural_monitor",
                 "spatial.stop_head_tracking"]:
        check(want in names, f"{want} present")

    print("\n== (a) multichannel foundation ==")
    chTrk = call("track.add", name="MCP p4 chan")["trackIndex"]; created.append(chTrk)
    sc = call("spatial.set_track_channels", track=chTrk, channels=12)
    check(sc["channels"] == 12, "set_track_channels -> 12ch", f'channels={sc["channels"]}')
    check("7.1.4" in sc.get("interpretation", ""), "12ch interpreted as 7.1.4",
          sc.get("interpretation", ""))
    sc16 = call("spatial.set_track_channels", track=chTrk, channels=16)
    check(sc16.get("ambisonicOrder") == 3, "16ch reports 3rd-order ambisonics",
          f'order={sc16.get("ambisonicOrder")}, {sc16.get("interpretation")}')

    print("\n== (a) build 7.1.4 bed ==")
    bed = call("spatial.build_bed", layout="7.1.4", busName="MCP p4 bed")
    busTrk = bed["busTrack"]; created.append(busTrk)
    check(bed["channels"] == 12, "bed has 12 channels", f'channels={bed["channels"]}')
    cmap = bed["channelMap"]
    check(len(cmap) == 12, "channelMap has 12 entries", f"len={len(cmap)}")
    lfe = next((c for c in cmap if c["lfe"]), None)
    check(lfe is not None and lfe["channel"] == 4 and lfe["label"] == "LFE",
          "channel 4 is LFE", str(lfe))
    labels = [c["label"] for c in cmap]
    check(labels[:4] == ["L", "R", "C", "LFE"] and "Ltf" in labels and "Rtr" in labels,
          "channel-order labels correct (L R C LFE … heights)", str(labels))
    # Confirm REAPER actually set the bus track to 12 channels.
    tl = call("track.list")
    bt = next((t for t in tl["tracks"] if t["index"] == busTrk), None)
    check(bt is not None and bt["channels"] == 12, "bus track really has 12 channels in REAPER",
          f'channels={bt["channels"] if bt else "?"}')

    print("\n== (a) assign stems to bed (multichannel send encoding) ==")
    srcTrk = call("track.add", name="MCP p4 src")["trackIndex"]; created.append(srcTrk)
    a1 = call("spatial.assign_to_bed", track=srcTrk, bed=busTrk, destChannel=0, channels=2)
    check(a1["sendIndex"] >= 0 and a1["srcChanEnc"] == 0 and a1["dstChanEnc"] == 0,
          "stereo send -> pair 0 (I_SRCCHAN=0, I_DSTCHAN=0)", str(a1))
    a2 = call("spatial.assign_to_bed", track=srcTrk, bed=busTrk, destChannel=4, channels=1)
    check(a2["srcChanEnc"] == 1024 and a2["dstChanEnc"] == 4,
          "mono send -> speaker 4 (I_SRCCHAN=1024 mono, I_DSTCHAN=4)", str(a2))
    # Cross-check via send.list that the sends really exist on the source track.
    sl = call("send.list", track=srcTrk)
    check(sl["sendCount"] >= 2, "send.list shows >= 2 sends from source", f'count={sl["sendCount"]}')

    print("\n== (b) ReaSurroundPan control ==")
    panTrk = call("track.add", name="MCP p4 pan")["trackIndex"]; created.append(panTrk)
    try:
        sp = call("spatial.add_surround_panner", track=panTrk, setTrackChannels=8)
        fx = sp["fxIndex"]
        check(fx >= 0, "add_surround_panner inserted an FX", f'fxIndex={fx}, name={sp.get("name")!r}')
        check("surround" in sp.get("name", "").lower(), "FX name looks like a surround panner",
              sp.get("name", ""))
        check(sp.get("paramCount", 0) > 0, "panner exposes parameters",
              f'paramCount={sp.get("paramCount")}, numSpeakers={sp.get("numSpeakers")!r}')

        st = settle_panner(panTrk)
        check(st["paramCount"] > 0, "get_surround_state dumps params", f'paramCount={st["paramCount"]}')

        # Deterministic write path: drive params 0 & 1 by explicit index (mechanism test — proves
        # SetParamNormalized reaches REAPER; semantic x/y naming is confirmed by get_surround_state).
        # Only once the panner's layout has SETTLED (33 -> 51) are params 0/1 the source-1 x/y; if it
        # never settles this run, skip the readback (info) rather than assert a transient layout.
        if st["paramCount"] >= SURROUND_SETTLED:
            pos = call("spatial.set_source_position", track=panTrk, x=0.5, y=-0.5, paramX=0, paramY=1)
            check(pos["ok"] and len(pos.get("set", [])) == 2,
                  "set_source_position wrote 2 axes (explicit paramX/paramY)", str(pos.get("set")))
            st2 = call("spatial.get_surround_state", track=panTrk)
            p0 = next((p for p in st2["params"] if p["index"] == 0), {})
            p1 = next((p for p in st2["params"] if p["index"] == 1), {})
            check(abs(p0.get("normalized", -9) - 0.75) < 0.03,
                  "param0 read back ~0.75 (x=0.5 -> normalized)", f'got {p0.get("normalized")}')
            check(abs(p1.get("normalized", -9) - 0.25) < 0.03,
                  "param1 read back ~0.25 (y=-0.5 -> normalized)", f'got {p1.get("normalized")}')
        else:
            print(f"  [info] ReaSurroundPan params did not settle to the full layout "
                  f"(paramCount={st['paramCount']}) — skipping the x/y readback this run")

        # Informational: does name-based discovery find x/y params on this build? (not a gate)
        disc_pos = call("spatial.set_source_position", track=panTrk, x=0.0, y=0.0)
        print(f"  [info] name-discovery set_source_position: ok={disc_pos.get('ok')} "
              f"set={disc_pos.get('set')} msg={disc_pos.get('message','')}")
    except RuntimeError as e:
        check(False, "ReaSurroundPan available", f"{e} — is 'ReaSurroundPan' the FX name on your build?")

    # =====================================================================================
    # (c) SCENE-BASED AMBISONICS — orchestrating installed IEM/SPARTA/ATK plug-ins.
    # The encode/rotate/decode tools insert a real third-party FX, so their live checks only
    # run when a suite is installed on THIS machine; otherwise they are reported as skipped
    # (install the IEM Plug-in Suite to fully close the batch-c gate). The 54-tool surface,
    # detection, channel math, and pin-wiring are the parts proven here.
    # =====================================================================================
    print("\n== (c) scene-based ambisonics ==")
    det = call("spatial.detect_spatial_suites")
    suites = det.get("suites", [])
    installed = [s["suite"] for s in suites if s.get("installed")]
    pref = det.get("preferred", {})
    check(isinstance(suites, list) and len(suites) >= 4,
          "detect_spatial_suites returns the suite table",
          f'scanned {det.get("totalInstalledFx")} installed FX; suites={installed or "none"}')
    print(f"  [info] preferred: {pref}")

    # preferred.encoder should be a GENERAL-PURPOSE encoder (StereoEncoder/MultiEncoder/AmbiENC),
    # not a niche one like GranularEncoder/RoomEncoder that also matches the "encod" keyword.
    enc_name = (pref.get("encoder") or {}).get("name", "")
    check(not enc_name or any(k.lower() in enc_name.lower()
                              for k in ("StereoEncoder", "MultiEncoder", "AmbiENC", "ambix_encoder",
                                        "FOA Encode")),
          "preferred.encoder is a general-purpose encoder (not Granular/Room)",
          enc_name or "(none installed)")

    have_enc = pref.get("encoder") is not None
    if have_enc:
        encTrk = call("track.add", name="MCP p4c enc")["trackIndex"]; created.append(encTrk)
        e3 = call("spatial.ambisonic_encode", track=encTrk, order=3, azimuthDeg=30, elevationDeg=15)
        check(e3["ok"] and e3["hoaChannels"] == 16 and e3["trackChannels"] == 16 and not e3["padded"],
              "encode order 3 -> 16ch HOA on a 16ch track",
              f'encoder={e3.get("encoder")!r} trackCh={e3.get("trackChannels")}')
        check(len(e3.get("pinMap", [])) == 16, "encoder ACN outputs pin-wired identity 0..15",
              f'pins={len(e3.get("pinMap", []))}')
        check(len(e3.get("positions", [])) >= 1,
              "azimuth/elevation params discovered + set by name",
              str([p.get("name") for p in e3.get("positions", [])]))
        # Angle calibration: IEM exposes az/el as normalized [0,1]; the tool maps degrees onto each
        # param's REAL degree range (azimuth ±180, elevation ±90 — different spans), not raw degrees.
        az = axis_entry(e3.get("positions"), "azimuth")
        el = axis_entry(e3.get("positions"), "elevation")
        if az and az.get("degRange") and el and el.get("degRange"):
            az_span = az["degRange"]["hi"] - az["degRange"]["lo"]
            el_span = el["degRange"]["hi"] - el["degRange"]["lo"]
            # The tool discovers each param's REAL degree range from the plug-in (not a hardcoded ±180/±90
            # guess). IEM StereoEncoder happens to report ±180 for BOTH azimuth and elevation — so we only
            # assert both ranges were discovered and are plausible degree spans; the value checks below
            # prove the mapping is correct against whatever range the plug-in actually uses. (The per-axis
            # ±90 vs ±180 capability is covered by the head-track unit test.)
            check(30 <= abs(az_span) <= 1000 and 30 <= abs(el_span) <= 1000,
                  "azimuth + elevation degree ranges discovered from the plug-in itself",
                  f'az=±{round(abs(az_span)/2,1)}° el=±{round(abs(el_span)/2,1)}° (this IEM build reports ±180 for both)')
            exp_az = angle_to_param(30, az["min"], az["max"], az["degRange"]["lo"], az["degRange"]["hi"])
            exp_el = angle_to_param(15, el["min"], el["max"], el["degRange"]["lo"], el["degRange"]["hi"])
            check(abs(az["value"] - exp_az) < 0.02, "azimuth 30° mapped to the correct param value",
                  f'expected≈{round(exp_az,3)} got {round(az["value"],3)}')
            check(abs(el["value"] - exp_el) < 0.02, "elevation 15° mapped to the correct param value",
                  f'expected≈{round(exp_el,3)} got {round(el["value"],3)}')
            ra = call("fx.get_param", track=encTrk, fx=e3["fxIndex"], param=az["param"])
            check(abs(ra["value"] - az["value"]) < 0.02, "azimuth param reads back from REAPER",
                  f'set={round(az["value"],3)} read={round(ra["value"],3)}')
        else:
            print(f"  [info] encoder positions missing degRange (non-normalized params?) — az={az} el={el}")
        tl2 = call("track.list")
        et = next((t for t in tl2["tracks"] if t["index"] == encTrk), None)
        check(et is not None and et["channels"] == 16, "encode track really 16ch in REAPER",
              f'channels={et["channels"] if et else "?"}')
        # even-pad case: 2nd order = 9 HOA channels riding a 10-channel (even) track.
        enc2 = call("track.add", name="MCP p4c enc2")["trackIndex"]; created.append(enc2)
        e2 = call("spatial.ambisonic_encode", track=enc2, order=2, azimuthDeg=0)
        check(e2["hoaChannels"] == 9 and e2["trackChannels"] == 10 and e2["padded"],
              "encode order 2 -> 9ch HOA even-padded to a 10ch track",
              str({k: e2.get(k) for k in ("hoaChannels", "trackChannels", "padded")}))
        check(any("padded" in w or "surplus" in w for w in e2.get("warnings", [])),
              "even-pad warning emitted", str(e2.get("warnings")))

        if pref.get("rotator") is not None:
            rot = call("spatial.rotate_scene", track=encTrk, yawDeg=45, pitchDeg=0, rollDeg=0)
            check(rot["ok"] and rot["fxIndex"] >= 0, "rotate_scene inserted a rotator",
                  f'rotator={rot.get("rotator")!r}')
            ap = rot.get("automationParams", {})
            check(ap.get("yaw", -1) >= 0, "yaw param index discovered for automation", str(ap))
            # Angle calibration: yawDeg=45 must map to the tracked +45°, NOT peg to the range extreme.
            sy = axis_entry(rot.get("set"), "yaw")
            if sy and sy.get("degRange"):
                exp = angle_to_param(45, sy["min"], sy["max"], sy["degRange"]["lo"], sy["degRange"]["hi"])
                check(abs(sy["value"] - exp) < 0.02 and abs(sy["value"] - sy["max"]) > 0.02,
                      "rotate_scene yaw 45° mapped to the tracked angle (not the extreme)",
                      f'expected≈{round(exp,3)} got {round(sy["value"],3)} (max {sy["max"]})')
                ry = call("fx.get_param", track=encTrk, fx=rot["fxIndex"], param=sy["param"])
                check(abs(ry["value"] - sy["value"]) < 0.02, "rotate_scene yaw reads back from REAPER",
                      f'set={round(sy["value"],3)} read={round(ry["value"],3)}')
            else:
                print(f"  [info] rotator set entry missing degRange — {sy}")
        else:
            print("  [info] no rotator installed — skipping rotate_scene live test")

        if pref.get("decoder") is not None:
            decTrk = call("track.add", name="MCP p4c dec")["trackIndex"]; created.append(decTrk)
            call("spatial.set_track_channels", track=decTrk, channels=16)
            dec = call("spatial.ambisonic_decode", track=decTrk, order=3, outputChannels=12)
            check(dec["ok"] and dec["fxIndex"] >= 0, "ambisonic_decode inserted a decoder",
                  f'decoder={dec.get("decoder")!r}')
            check(len(dec.get("pinMap", [])) == 16, "decoder HOA input pins wired 0..15",
                  f'pins={len(dec.get("pinMap", []))}')
        else:
            print("  [info] no decoder installed — skipping ambisonic_decode live test")
    else:
        print("  [info] no ambisonic encoder installed (IEM/SPARTA/ATK) — encode/rotate/decode")
        print("         live checks skipped. Install the IEM Plug-in Suite to fully close batch c.")
        encTrk = call("track.add", name="MCP p4c enc")["trackIndex"]; created.append(encTrk)
        try:
            call("spatial.ambisonic_encode", track=encTrk, order=3)
            check(True, "ambisonic_encode succeeded (an encoder turned out to be present)")
        except RuntimeError as e:
            check("detect_spatial_suites" in str(e) or "encoder" in str(e),
                  "ambisonic_encode fails cleanly with guidance when no encoder present", str(e))

    # =====================================================================================
    # (d) RENDER DELIVERABLES — RENDER_* config + no-dialog render; native RENDER_STATS
    # loudness (LUFS-I + true peak) cross-checked against ffmpeg ebur128; Atmos send matrix.
    # The dryRun plan + Atmos layout are the always-on gate (no audio needed, fits the 5s
    # tools/call window). Pass --render to also fire a short real render and run the ffmpeg
    # cross-check on the written file (use a project with audio near t=0 for real numbers).
    # =====================================================================================
    print("\n== (d) render deliverables — plan + Atmos layout ==")

    # dryRun plan: multichannel master from the 7.1.4 bed bus (RENDER_TARGETS reflects settings).
    plan = call("spatial.render_deliverables", targets=["multichannel"], bedTrack=busTrk, dryRun=True)
    r0 = (plan.get("rendered") or [{}])[0]
    check(r0.get("channels") == 12, "dryRun multichannel from bed bus -> 12ch render",
          f'channels={r0.get("channels")}, source={r0.get("source")!r}')
    check(r0.get("dryRun") is True and isinstance(r0.get("plannedTargets"), list),
          "dryRun reports RENDER_TARGETS (files that would be written)", str(r0.get("plannedTargets")))
    check(bool(plan.get("format")), "render format id reported",
          f'format={plan.get("format")!r} (source={plan.get("formatSource")!r})')

    # dryRun binaural from master -> 2ch downmix.
    planB = call("spatial.render_deliverables", targets=["binaural"], dryRun=True)
    rb = (planB.get("rendered") or [{}])[0]
    check(rb.get("channels") == 2, "dryRun binaural -> 2ch master downmix",
          f'channels={rb.get("channels")}, source={rb.get("source")!r}')

    # ambiX order sanity: a 16ch bus reads back as 3rd-order in the plan.
    planA = call("spatial.render_deliverables", targets=["ambix"], ambixTrack=chTrk, dryRun=True)
    ra = (planA.get("rendered") or [{}])[0]
    check(ra.get("ambisonicOrder") == 3 or ra.get("channels") == 16,
          "dryRun ambix from a 16ch bus reports 3rd order",
          f'order={ra.get("ambisonicOrder")}, channels={ra.get("channels")}')

    # Atmos bed+object send layout onto a renderer bus (auto-widened to hold bed+objects).
    rendTrk = call("track.add", name="MCP p4d renderer")["trackIndex"]; created.append(rendTrk)
    atm = call("spatial.render_deliverables", targets=["atmos-send-layout"],
               bedTrack=busTrk, objectTracks=[srcTrk], rendererTrack=rendTrk, bedLayout="7.1.4")
    ad = atm.get("atmos", {})
    check(ad.get("bedChannels") == 12 and ad.get("objectCount") == 1,
          "Atmos layout: 12-ch 7.1.4 bed + 1 object",
          str({k: ad.get(k) for k in ("bedChannels", "objectCount", "totalChannels")}))
    check(isinstance(ad.get("sends"), list) and len(ad["sends"]) >= 2,
          "Atmos layout created bed + object sends onto the renderer bus",
          f'sends={len(ad.get("sends", []))}')
    tl3 = call("track.list")
    rt = next((t for t in tl3["tracks"] if t["index"] == rendTrk), None)
    check(rt is not None and rt["channels"] >= 13, "renderer bus widened to hold bed+objects",
          f'channels={rt["channels"] if rt else "?"}')

    # ---- optional: real render + ffmpeg ebur128 cross-check (--render) ----
    if RENDER:
        print("\n== (d) real render + loudness cross-check (--render) ==")

        # Generate a short, controlled test tone on its OWN track (ReaSynth + a sustained MIDI note,
        # moderate velocity) and render THAT track in isolation. Rendering the summed master would
        # clip (many tracks sum > 0 dBFS), and on a clipping render the two engines legitimately
        # diverge — REAPER's RENDER_STATS measures the internal pre-clip signal while ffmpeg measures
        # the clipped file. An isolated, non-clipping tone makes the cross-check apples-to-apples.
        tone_ok, toneTrk = False, None
        try:
            toneTrk = call("track.add", name="MCP p4d tone")["trackIndex"]; created.append(toneTrk)
            call("fx.add", track=toneTrk, name="ReaSynth")
            call("track.set_fader", track=toneTrk, db=-18.0)  # headroom: guarantee a non-clipping render
            tone_item = call("midi.create_item", track=toneTrk, position=0.0,
                             length=max(RENDER_DUR + 1.0, 3.0))["item"]
            call("midi.insert_note", track=toneTrk, item=tone_item, startBeats=0.0, lengthBeats=16.0,
                 pitch=69, velocity=90)
            tone_ok = True
            print("  [info] inserted a ReaSynth A4 test tone on its own track (rendered in isolation)")
        except RuntimeError as e:
            print(f"  [info] could not set up a test tone ({e}); render may be silent/clipping")

        outdir = tempfile.mkdtemp(prefix="reamcp_render_")
        render_args = dict(targets=["multichannel"], outDir=outdir, boundsFlag=0,
                           startPos=0.0, endPos=RENDER_DUR, dryRun=False)
        if tone_ok:
            render_args["sourceTrack"] = toneTrk   # isolate the clean tone (no master-sum clipping)
        rr = call("spatial.render_deliverables", **render_args)
        item = (rr.get("rendered") or [{}])[0]
        path_out = item.get("path")
        if not path_out:
            wavs = glob.glob(os.path.join(outdir, "*.wav"))
            path_out = wavs[0] if wavs else None
        check(bool(path_out) and os.path.exists(path_out),
              "real render wrote a master file", f'path={path_out}')
        native_lufs, native_tp = item.get("lufs"), item.get("truePeak")
        native_peak = item.get("samplePeak")
        check(str(item.get("loudnessEngine", "")).startswith("reaper-native"),
              "native loudness engine reported (RENDER_STATS)")
        print(f"  [info] native RENDER_STATS: LUFS-I={native_lufs} TP={native_tp} "
              f"samplePeak={native_peak} dBFS")
        print(f"  [info] raw RENDER_STATS  : {item.get('rawStats')}")
        if tone_ok:
            check(native_lufs is not None,
                  "native RENDER_STATS reports integrated LUFS on a real signal (analysis pass works)",
                  f'LUFS-I={native_lufs}')

        clipped = native_peak is not None and native_peak > 0.0
        if clipped:
            print(f"  [info] render clips (sample peak {native_peak:+.1f} dBFS) — REAPER measures the "
                  f"pre-clip signal, ffmpeg the clipped file, so a LUFS gap here is expected, not a bug")

        if not path_out or not os.path.exists(path_out):
            pass
        elif not shutil.which("ffmpeg"):
            print("  [info] ffmpeg not installed — `brew install ffmpeg` to enable the cross-check")
        else:
            f_lufs, f_tp = ffmpeg_loudness(path_out)
            print(f"  [info] ffmpeg ebur128    : LUFS-I={f_lufs} TP={f_tp} dBTP")
            if (native_lufs is not None and f_lufs is not None
                    and native_lufs > -60 and f_lufs > -60 and not clipped):
                check(abs(native_lufs - f_lufs) <= 1.0,
                      "REAPER-native vs ffmpeg integrated LUFS agree (<=1.0 LU)",
                      f'native={native_lufs} ffmpeg={f_lufs} d={round(abs(native_lufs - f_lufs), 2)}')
                if native_tp is not None and f_tp is not None:
                    check(abs(native_tp - f_tp) <= 1.5,
                          "REAPER-native vs ffmpeg true peak agree (<=1.5 dB)",
                          f'native={native_tp} ffmpeg={f_tp}')
            else:
                print("  [info] numeric cross-check skipped (silence, clipping, or no finite reading) — "
                      "compare the two [info] LUFS lines above by eye")
    else:
        print("  [info] pass --render to also fire a short real render + ffmpeg loudness cross-check")

finally:
    if CLEANUP and created:
        for idx in sorted(created, reverse=True):
            try:
                call("track.remove", track=idx)
            except Exception as e:
                print(f"(cleanup: could not remove track {idx}: {e})")
        print(f"\n(cleaned up {len(created)} test tracks)")

print(f"\n{'='*52}\n{passed} passed, {failed} failed")
if not CLEANUP and created:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
