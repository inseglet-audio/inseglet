#!/usr/bin/env python3
"""Regression gate for Bug C: the shared FX-resolve substrate must REUSE an on-track instance
instead of stacking a fresh one on every call.

resolveOrAddByCandidates() (src/tools/tools_spatial.cpp) backs spatial.ambisonic_encode /
rotate_scene / ambisonic_decode / convert_format / mirror_scene / beamform / set_distance. Before the
fix it always called TrackFX_AddByName(..., -1) (always instantiate), so repeated calls on one track
STACKED duplicate plug-ins (observed live: 6 set_distance calls -> 6 RoomEncoders). The fix reuses an
existing instance first (REAPER query-only resolve, instantiate=0 — which also folds a bundled JSFX by
its file alias — then a substring scan of on-track FX names), only adding fresh when none is present.

For each substrate tool this gate adds a dedicated FOA scratch track, calls the tool TWICE with NO
`fx` override (varying a param so it is a real re-drive), then asserts via fx.list that the track holds
EXACTLY ONE FX and that both calls returned the SAME fxIndex; a third call pinned with that fx must
also leave the count at 1. A tool whose plug-in is not installed (e.g. SPARTA Beamformer) is SKIPPED,
not failed. Each scratch track is removed. REAPER must be FOREGROUND. Stdlib only.

Usage:
    python3 verify_bugc.py                 # auto-find reaper_mcp.json
    python3 verify_bugc.py /path/reaper_mcp.json
"""
import json, os, sys, urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
args = [a for a in sys.argv[1:] if not a.startswith("--")]
path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found - pass its path, or run the 'reaper_mcp: status' action first.")
disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print("endpoint : %s\ntoken    : %s...\n" % (URL, TOKEN[:8]))

_id = 0
def next_id():
    global _id; _id += 1; return _id
def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method,
                       "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=60) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError("%s -> %s" % (method, j["error"]))
    return j["result"]
def call(tool, **kw):
    r = rpc("tools/call", {"name": tool, "arguments": kw})
    if r.get("isError"):
        raise RuntimeError("%s isError: %s" % (tool, r.get("content")))
    return r.get("structuredContent", {})
def raw(tool, **kw):
    return rpc("tools/call", {"name": tool, "arguments": kw})
def err_text(result):
    return " ".join(c.get("text", "") for c in result.get("content", []) if isinstance(c, dict))

rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "verify_bugc", "version": "0"}})
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}

def add_foa():
    base = call("project.get_summary").get("trackCount", 0)
    tr = call("track.add", index=base, name="BUGC VERIFY").get("trackIndex", base)
    call("spatial.convert_order", track=tr, toOrder=1)   # 4-ch FOA carrier
    return tr
def fx_list(tr):
    r = call("fx.list", track=tr)
    return r.get("fxCount", 0), r.get("fx", [])

results = []  # (label, status, detail)
def run_case(label, tool, a1, a2):
    tr = add_foa()
    try:
        p1 = raw(tool, track=tr, **a1)
        if p1.get("isError"):
            results.append((label, "SKIP", "fail-closed (plug-in absent?): " + err_text(p1)[:70]))
            return
        sc1 = p1.get("structuredContent", {})
        if not sc1.get("ok", False):
            results.append((label, "SKIP", "ok=false"))
            return
        fx1 = sc1.get("fxIndex")
        fx2 = call(tool, track=tr, **a2).get("fxIndex")
        n, lst = fx_list(tr)
        names = ", ".join(f.get("name", "?") for f in lst)
        if fx2 is not None:                      # explicit-fx path must reuse too
            call(tool, track=tr, fx=fx2, **a2)
        n3, _ = fx_list(tr)
        ok = (n == 1) and (fx1 == fx2) and (n3 == 1)
        results.append((label, "PASS" if ok else "FAIL",
                        "fxCount=%d (fx:%s call -> %d)  fxIndex %s==%s  [%s]" % (n, fx2, n3, fx1, fx2, names)))
    finally:
        try: call("track.remove", track=tr)
        except Exception: pass

run_case("set_distance (IEM RoomEncoder)", "spatial.set_distance",
         dict(distanceM=10.0, azimuthDeg=0.0), dict(distanceM=10.0, azimuthDeg=90.0))
run_case("ambisonic_encode (IEM StereoEncoder)", "spatial.ambisonic_encode",
         dict(order=1, azimuthDeg=0.0), dict(order=1, azimuthDeg=90.0))
run_case("rotate_scene (IEM SceneRotator)", "spatial.rotate_scene",
         dict(yawDeg=0.0), dict(yawDeg=90.0))
run_case("ambisonic_decode (IEM decoder)", "spatial.ambisonic_decode", dict(), dict())
run_case("convert_format (bundled JSFX)", "spatial.convert_format",
         {"from": "SN3D", "to": "N3D"}, {"from": "SN3D", "to": "N3D"})
run_case("mirror_scene (bundled JSFX)", "spatial.mirror_scene",
         dict(plane="left-right"), dict(plane="front-back"))
run_case("beamform (SPARTA Beamformer)", "spatial.beamform",
         dict(azimuthDeg=0.0), dict(azimuthDeg=90.0))

print("Bug C - FX-reuse regression (each tool called twice with NO fx override):\n")
passed = skipped = failed = 0
for label, status, detail in results:
    print("  [%-4s] %-40s %s" % (status, label, detail))
    passed += status == "PASS"; skipped += status == "SKIP"; failed += status == "FAIL"
print("\n  %d passed, %d skipped, %d failed" % (passed, skipped, failed))
print("  Pass = each tool re-drives its instance (fxCount stays 1); pre-fix every row stacked to 2.")
sys.exit(1 if failed else 0)
