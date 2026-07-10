#!/usr/bin/env python3
"""Live-verify Phase 4 batch (e) — binaural monitor + LIVE head-tracking.

Reads the endpoint + bearer token from the extension's discovery file (reaper_mcp.json), then:
  1. asserts the 55-tool surface + the two batch-e tools;
  2. builds a 3rd-order HOA scene track and adds a binaural monitor bus with headTracking on;
  3. proves the EXTENSION-HOSTED OSC bridge end-to-end: it UDP-sends synthetic head-tracker OSC
     (/yaw //pitch //roll, a 3-float /ypr, and a 4-float /quaternion) to the port the tool reported,
     then reads the rotator's yaw/pitch/roll parameters back via fx.get_param and checks they moved
     to the value clamped into each param's own [min,max] — a plugin-scaling-agnostic assertion of
     the whole UDP -> OSC-parse -> coalesce -> main-thread TrackFX_SetParam pipeline;
  4. releases the port with spatial.stop_head_tracking and checks the received-packet count.

Needs the IEM Plug-in Suite (BinauralDecoder + SceneRotator) installed; without it the monitor build
is reported SKIPPED (as batch c does for ambisonics). REAPER must be in the FOREGROUND — App Nap stalls
the Run() pump that applies the head-tracked orientation. Stdlib only (urllib + socket).

Usage:
    python3 verify_phase5.py                 # auto-find reaper_mcp.json
    python3 verify_phase5.py /path/to/reaper_mcp.json
    python3 verify_phase5.py --cleanup       # also delete the test tracks when done
    python3 verify_phase5.py --port=9100      # OSC port to request (default 9100)
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),  # macOS
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),                      # Linux
]

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
CLEANUP = "--cleanup" in flags
REQ_PORT = 9100
for _f in flags:
    if _f.startswith("--port="):
        try:
            REQ_PORT = int(_f.split("=", 1)[1])
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

# --- synthetic head-tracker: pack + send OSC 1.0 over UDP ---
def _osc_pad(b):
    return b + b"\0" * ((4 - len(b) % 4) % 4)
def _osc_str(s):
    return _osc_pad(s.encode() + b"\0")
def osc_msg(addr, *floats):
    m = _osc_str(addr) + _osc_str("," + "f" * len(floats))
    for f in floats:
        m += struct.pack(">f", float(f))
    return m
def send_osc(host, port, packet):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _ in range(3):          # a few datagrams (UDP is lossy) so at least one lands per tick
            s.sendto(packet, (host, port))
            time.sleep(0.02)
    finally:
        s.close()

def angle_to_param(deg, mn, mx):
    # Mirror src/head_track_bridge.h angleToParamValue: a normalized [0,1]-style rotator param
    # (span<=2) maps -180..180 -> 0..1 (0 deg -> center); a real-degree param is written through.
    if mx - mn <= 2.0:
        pos = max(0.0, min(1.0, (deg + 180.0) / 360.0))
        return mn + pos * (mx - mn)
    return max(mn, min(mx, deg)) if mx > mn else deg

def tol(mn, mx):
    span = (mx - mn) if mx > mn else 1.0
    return 0.02 * span + 0.01

created = []
try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 55, "tools/list returns 55 tools (54 + stop_head_tracking)", f"got {len(tools)}")
    check("spatial.add_binaural_monitor" in names, "spatial.add_binaural_monitor present")
    check("spatial.stop_head_tracking" in names, "spatial.stop_head_tracking present")

    # A binaural decoder must be installed for the live monitor path.
    det = call("spatial.detect_spatial_suites")
    pref = det.get("preferred", {})
    have_binaural = pref.get("binaural") is not None
    have_rotator = pref.get("rotator") is not None
    print(f"  [info] preferred binaural={ (pref.get('binaural') or {}).get('name') } "
          f"rotator={ (pref.get('rotator') or {}).get('name') }")

    if not have_binaural:
        print("\n== (e) binaural monitor ==")
        print("  [info] no binaural decoder installed (IEM BinauralDecoder / SPARTA Binauraliser) —")
        print("         install the IEM Plug-in Suite to fully close batch e. Verifying clean failure:")
        scn = call("track.add", name="MCP p5 scene")["trackIndex"]; created.append(scn)
        call("spatial.set_track_channels", track=scn, channels=16)
        try:
            call("spatial.add_binaural_monitor", track=scn)
            check(True, "add_binaural_monitor succeeded (a decoder turned out to be present)")
        except RuntimeError as e:
            check("decoder" in str(e) or "binauraliser" in str(e).lower(),
                  "add_binaural_monitor fails cleanly with guidance when no decoder present", str(e))
    else:
        print("\n== (e) binaural monitor (ambisonic) ==")
        scn = call("track.add", name="MCP p5 scene")["trackIndex"]; created.append(scn)
        call("spatial.set_track_channels", track=scn, channels=16)   # 3rd-order HOA carrier

        mon = call("spatial.add_binaural_monitor", track=scn, headTracking=True,
                   oscPort=REQ_PORT, oscBind="127.0.0.1")
        monTrk = mon["monitorTrack"]; created.append(monTrk)
        check(mon["ok"] and monTrk >= 0, "add_binaural_monitor built a monitor bus",
              f'monitorTrack={monTrk}, name={mon.get("monitorName")!r}')
        check(mon.get("busChannels") == 16, "monitor bus is 16ch (HOA carried to the decoder input pins)",
              f'busChannels={mon.get("busChannels")}')
        check(mon.get("inputPins") == 16, "decoder HOA input pins wired 0..15", f'inputPins={mon.get("inputPins")}')
        check("binaural" in mon.get("decoder", "").lower(),
              "an ambisonic->binaural decoder was inserted", mon.get("decoder", ""))
        # Confirm the monitor bus really exists at 16ch in REAPER.
        bt = next((t for t in call("track.list")["tracks"] if t["index"] == monTrk), None)
        check(bt is not None and bt["channels"] == 16, "monitor bus really 16ch in REAPER",
              f'channels={bt["channels"] if bt else "?"}')

        ht = mon.get("headTracking", {})
        ap = mon.get("automationParams", {})
        rotFx = mon.get("rotatorFx", -1)
        if not (have_rotator and ht.get("enabled") and rotFx >= 0 and ap.get("yaw", -1) >= 0):
            print("  [info] no rotator / head-track not enabled — skipping the live OSC round-trip.")
            print(f"         headTracking={ht}")
        else:
            oscPort = ht["oscPort"]
            check(oscPort > 0, "head-track OSC listener bound a port", f'oscPort={oscPort}')
            check(rotFx >= 0 and mon.get("rotator"), "a scene rotator was inserted for head-tracking",
                  f'rotator={mon.get("rotator")!r} fx={rotFx}')

            def read_param(pidx):
                return call("fx.get_param", track=monTrk, fx=rotFx, param=pidx)

            yawP, pitchP, rollP = ap.get("yaw", -1), ap.get("pitch", -1), ap.get("roll", -1)
            yq = read_param(yawP)  # capture the param's [min,max] to compute clamped expectations
            print(f"  [info] rotator yaw param: name={yq.get('name')!r} range=[{yq.get('min')},{yq.get('max')}] "
                  f"value={yq.get('value')}")

            ymin, ymax = yq.get("min", 0.0), yq.get("max", 1.0)
            print("\n== (e) LIVE head-tracking over OSC (extension-hosted UDP bridge) ==")
            # 1) /yaw 90 -> the param should move to the tracked +90 deg (0.75 on a [0,1] IEM param).
            send_osc("127.0.0.1", oscPort, osc_msg("/yaw", 90.0))
            time.sleep(0.5)  # a few Run() ticks (REAPER must be foreground)
            exp = angle_to_param(90.0, ymin, ymax)
            v1 = read_param(yawP)["value"]
            check(abs(v1 - exp) <= tol(ymin, ymax),
                  "/yaw 90 rotated the yaw param to the tracked +90° (not an extreme)",
                  f'expected≈{round(exp,3)} got {round(v1,3)} (range [{ymin},{ymax}])')

            # 2) /yaw 0 -> back to center (0.5 on a [0,1] param).
            send_osc("127.0.0.1", oscPort, osc_msg("/yaw", 0.0))
            time.sleep(0.5)
            exp0 = angle_to_param(0.0, ymin, ymax)
            v0 = read_param(yawP)["value"]
            check(abs(v0 - exp0) <= tol(ymin, ymax),
                  "/yaw 0 returned the yaw param to center", f'expected≈{round(exp0,3)} got {round(v0,3)}')

            # 3) 3-float /ypr drives all three axes at once, each to its own tracked angle.
            if pitchP >= 0 and rollP >= 0:
                pq, rq = read_param(pitchP), read_param(rollP)
                send_osc("127.0.0.1", oscPort, osc_msg("/ypr", 45.0, 10.0, -20.0))
                time.sleep(0.5)
                ey = angle_to_param(45.0, yq["min"], yq["max"]); ep = angle_to_param(10.0, pq["min"], pq["max"])
                er = angle_to_param(-20.0, rq["min"], rq["max"])
                gy, gp, gr = read_param(yawP)["value"], read_param(pitchP)["value"], read_param(rollP)["value"]
                check(abs(gy - ey) <= tol(yq["min"], yq["max"]) and abs(gp - ep) <= tol(pq["min"], pq["max"])
                      and abs(gr - er) <= tol(rq["min"], rq["max"]),
                      "/ypr drove yaw+pitch+roll together to their tracked angles",
                      f'y:{round(gy,3)}~{round(ey,3)} p:{round(gp,3)}~{round(ep,3)} r:{round(gr,3)}~{round(er,3)}')

            # 4) 4-float /quaternion for +90 deg yaw (cos45,0,0,sin45) -> yaw to the tracked +90 deg.
            h = 0.7071067811865476
            send_osc("127.0.0.1", oscPort, osc_msg("/quaternion", h, 0.0, 0.0, h))
            time.sleep(0.5)
            vq = read_param(yawP)["value"]
            check(abs(vq - exp) <= tol(ymin, ymax) * 1.5,
                  "/quaternion (yaw +90) rotated the yaw param via the quaternion->Euler path",
                  f'expected≈{round(exp,3)} got {round(vq,3)}')

            # 5) Release the listener; it should report having received our packets.
            st = call("spatial.stop_head_tracking")
            check(st.get("ok") and st.get("wasRunning") is True, "stop_head_tracking stopped a running listener",
                  str(st))
            check(st.get("packetsReceived", 0) >= 4, "the OSC listener received the head-tracker packets",
                  f'packetsReceived={st.get("packetsReceived")}')

        # stop is idempotent even if head-tracking was skipped.
        st2 = call("spatial.stop_head_tracking")
        check(st2.get("ok") is True, "stop_head_tracking is safe to call again (idempotent)", str(st2))

        print("\n== (e) object-mode binaural monitor ==")
        # Object mode: two mono object tracks -> one binauraliser. Only runs if a binauraliser is present.
        o1 = call("track.add", name="MCP p5 obj1")["trackIndex"]; created.append(o1)
        o2 = call("track.add", name="MCP p5 obj2")["trackIndex"]; created.append(o2)
        try:
            om = call("spatial.add_binaural_monitor", track=o1, mode="object", objectTracks=[o1, o2])
            created.append(om["monitorTrack"])
            check(om["ok"] and om.get("mode") == "object", "object-mode monitor built",
                  f'decoder={om.get("decoder")!r}')
            check(om.get("inputPins") == 2, "one input pin per object (2)", f'inputPins={om.get("inputPins")}')
            check(len(om.get("sends", [])) == 2, "one send per object track", f'sends={len(om.get("sends", []))}')
        except RuntimeError as e:
            check("binauraliser" in str(e).lower() or "decoder" in str(e),
                  "object mode fails cleanly when no binauraliser present (install SPARTA)", str(e))

finally:
    # Always release any listener we might have left running, then optionally clean up tracks.
    try:
        call("spatial.stop_head_tracking")
    except Exception:
        pass
    if CLEANUP and created:
        for idx in sorted(set(created), reverse=True):
            try:
                call("track.remove", track=idx)
            except Exception as e:
                print(f"(cleanup: could not remove track {idx}: {e})")
        print(f"\n(cleaned up {len(set(created))} test tracks)")

print(f"\n{'='*52}\n{passed} passed, {failed} failed")
if not CLEANUP and created:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
