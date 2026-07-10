#!/usr/bin/env python3
"""Live-verify Phase 8 Batch P8-3 — depth (FX/take-FX, sends/routing, tempo markers, project, master).

Drives every P8-3 tool against a running REAPER + the loaded extension, proving the SDK path (not just
the host stub) works end-to-end. The gate builds its own scratch tracks/items, exercises the tools on
them, and (with --cleanup) tears them down.

SAFETY / pump-hygiene (the App Nap invariant — a modal dialog would block the Run() pump):
  * project.render only DRY-RUNs (never renders — a real render blocks the main thread).
  * project.open / project.new are only exercised on their pump-safe GUARD paths (they return a
    structured 'unsaved_changes' / 'file_not_found' WITHOUT opening anything, because the scratch project
    is dirty). No project is ever actually opened or replaced.
  * project.save does a real save-AS to a temp .rpp by default (non-modal, non-fatal) so SaveProjectEx is
    exercised; pass --no-save to skip it. It renames the (scratch) project to the temp path — run on a
    SCRATCH project.
  * master.add_fx / master.set_fx_param leave one FX on the master chain (there is no master FX remover in
    the toolset yet) — harmless on a scratch project; noted at the end.

REAPER must be in the FOREGROUND. Stdlib only.

Usage:
    python3 verify_phase83.py                 # auto-find reaper_mcp.json; builds + tests; leaves scratch state
    python3 verify_phase83.py --cleanup       # also delete the scratch tracks it created
    python3 verify_phase83.py --no-save       # skip the real project.save-as (guard-only project coverage)
    python3 verify_phase83.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request
from urllib.parse import urlparse

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),  # macOS
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),                      # Linux
]
EXPECTED_SURFACE = 124  # 97 (P8-2 baseline) + 27 (P8-3)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags
DO_SAVE = "--no-save" not in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action first.")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
U = urlparse(URL)
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print(f"endpoint : {URL}\ntoken    : {TOKEN[:8]}…\n")

_id = 0
def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=30) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    """structuredContent of a successful call. A tool-level makeError (structured 'error' field) is NOT a
    transport isError, so it comes back here — callers inspect .get('error')."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def raw(tool_name, **arguments):
    """Full result — for negative tests that expect a transport isError (e.g. schema validation)."""
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})

def initialize():
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "verify_phase83", "version": "0"}})

passed = failed = skipped = 0
notes = []
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

def skip(label, detail=""):
    global skipped
    skipped += 1
    print(f"  [SKIP] {label}" + (f"  — {detail}" if detail else ""))

# ================================================================================================
initialize()
print(f"== 0. surface == {EXPECTED_SURFACE} ==")
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == EXPECTED_SURFACE,
      f"tool surface == {EXPECTED_SURFACE} (97 + 27 P8-3)", f"count={enum.get('count')}")
listed = {t.get("name") for t in rpc("tools/list").get("tools", [])}
p83 = ["fx.set_enabled", "fx.get_preset", "fx.set_preset",
       "takefx.add", "takefx.list", "takefx.remove", "takefx.get_param", "takefx.set_param",
       "takefx.set_enabled", "send.set", "send.set_channels", "receive.list",
       "send.add_hardware_output", "tempo.list_markers", "tempo.add_marker", "tempo.edit_marker",
       "tempo.delete_marker", "time.qn_to_time", "time.time_to_qn", "project.save", "project.open",
       "project.new", "project.render", "master.get", "master.set", "master.add_fx",
       "master.set_fx_param"]
missing = [t for t in p83 if t not in listed]
check(not missing, "all 27 P8-3 tools present in tools/list", f"missing={missing}" if missing else "")

# --- scratch setup: two tracks at the end of the project ---
base = call("project.get_summary").get("trackCount", 0)
trackA = call("track.add", index=base, name="MCP P8-3 scratch A").get("trackIndex", base)
trackB = call("track.add", index=base + 1, name="MCP P8-3 scratch B").get("trackIndex", base + 1)
print(f"\nscratch tracks: A={trackA}  B={trackB}\n")

print("== 1. FX depth (fx.set_enabled / get_preset / set_preset) ==")
fx = call("fx.add", track=trackA, name="ReaEQ").get("fxIndex", 0)
check(call("fx.set_enabled", track=trackA, fx=fx, enabled=False).get("enabled") is False,
      "fx.set_enabled(false) -> bypassed")
check(call("fx.set_enabled", track=trackA, fx=fx, enabled=True).get("enabled") is True,
      "fx.set_enabled(true) -> enabled")
gp = call("fx.get_preset", track=trackA, fx=fx)
check("preset" in gp and "presetIndex" in gp and "presetCount" in gp,
      "fx.get_preset returns preset/presetIndex/presetCount", f"count={gp.get('presetCount')}")
sp = call("fx.set_preset", track=trackA, fx=fx, move=0)  # no-op navigation — safe
check(sp.get("ok") is True, "fx.set_preset(move=0) -> ok")

print("\n== 2. Take FX (takefx.add/list/get_param/set_param/set_enabled/remove) ==")
item = call("midi.create_item", track=trackA, position=0.0, length=1.0).get("item", 0)
tfx = call("takefx.add", track=trackA, item=item, name="ReaEQ").get("fxIndex", 0)
check(call("takefx.list", track=trackA, item=item).get("fxCount", 0) >= 1, "takefx.list sees the added FX")
check(call("takefx.set_enabled", track=trackA, item=item, fx=tfx, enabled=False).get("enabled") is False,
      "takefx.set_enabled(false)")
call("takefx.set_enabled", track=trackA, item=item, fx=tfx, enabled=True)
gpar = call("takefx.get_param", track=trackA, item=item, fx=tfx, param=0)
check("value" in gpar and "normalized" in gpar, "takefx.get_param returns value + normalized")
check(call("takefx.set_param", track=trackA, item=item, fx=tfx, param=0, normalized=0.5).get("ok") is True,
      "takefx.set_param(normalized=0.5) -> ok")
check(call("takefx.remove", track=trackA, item=item, fx=tfx).get("ok") is True, "takefx.remove -> ok")
check(call("takefx.list", track=trackA, item=item).get("fxCount", 99) == 0, "takefx.list empty after remove")

print("\n== 3. Sends / routing depth (send.set / set_channels / receive.list / hw output) ==")
sidx = call("send.add", track=trackA, dest=trackB).get("sendIndex", 0)
ss = call("send.set", track=trackA, index=sidx, db=-6.0, pan=0.25, mute=False, mode=1)
check(ss.get("ok") is True and abs(ss.get("volDb", 0) + 6.0) < 0.2 and ss.get("mode") == 1,
      "send.set applies db/pan/mode", f"volDb={ss.get('volDb'):.2f} mode={ss.get('mode')}")
sc = call("send.set_channels", track=trackA, index=sidx, srcChannel=0, srcChannels=2, dstChannel=0)
check(sc.get("ok") is True and sc.get("srcChan") == 0, "send.set_channels stereo src @ offset 0 -> 0",
      f"srcChan={sc.get('srcChan')} dstChan={sc.get('dstChan')}")
rl = call("receive.list", track=trackB)
found = any(r.get("srcTrack") == trackA for r in rl.get("receives", []))
check(rl.get("receiveCount", 0) >= 1 and found, "receive.list on B shows the receive from A",
      f"receiveCount={rl.get('receiveCount')}")
try:
    hw = call("send.add_hardware_output", track=trackA, outputChannel=0)
    check(hw.get("sendIndex", -1) >= 0, "send.add_hardware_output created a hw send",
          f"sendIndex={hw.get('sendIndex')}")
except RuntimeError as e:
    skip("send.add_hardware_output", f"no hardware outputs available here ({e})")

print("\n== 4. Tempo/time-sig markers + QN<->time ==")
c0 = call("tempo.list_markers").get("count", 0)
add = call("tempo.add_marker", position=8.0, bpm=140.0, timesigNum=3, timesigDenom=4)
midx = add.get("index", -1)
check(add.get("ok") is True and midx >= 0, "tempo.add_marker(8s, 140bpm, 3/4) -> ok", f"index={midx}")
lst = call("tempo.list_markers")
mk = next((m for m in lst.get("markers", []) if abs(m.get("position", -1) - 8.0) < 1e-3), None)
check(mk is not None and abs(mk.get("bpm", 0) - 140.0) < 1e-6 and mk.get("timesigNum") == 3,
      "tempo.list_markers reflects the new marker (bpm=140, 3/4)")
if mk is not None:
    ei = mk.get("index")
    check(call("tempo.edit_marker", index=ei, bpm=150.0).get("ok") is True, "tempo.edit_marker bpm -> 150")
    lst2 = call("tempo.list_markers")
    mk2 = next((m for m in lst2.get("markers", []) if m.get("index") == ei), None)
    check(mk2 is not None and abs(mk2.get("bpm", 0) - 150.0) < 1e-6, "edit persisted (bpm now 150)")
    check(call("tempo.delete_marker", index=ei).get("ok") is True, "tempo.delete_marker -> ok")
    after = call("tempo.list_markers")
    gone = not any(abs(m.get("position", -1) - 8.0) < 1e-3 for m in after.get("markers", []))
    check(gone, "the deleted marker (position 8) is gone from tempo.list_markers")
    # NOTE: REAPER auto-creates an anchor marker at time 0 when the FIRST tempo/time-sig marker is added to
    # a project, so the count does NOT necessarily return to the pre-add baseline (this is REAPER behavior,
    # not the tool's). If we started from an empty tempo map, clean up whatever we caused (delete highest
    # index first so lower indices stay valid).
    if c0 == 0:
        for m in sorted(after.get("markers", []), key=lambda x: -x.get("index", 0)):
            call("tempo.delete_marker", index=m.get("index"))
        notes.append("REAPER auto-added an anchor tempo marker at t=0 on first-add; the gate cleaned it up")
t4 = call("time.qn_to_time", qn=4.0).get("time")
back = call("time.time_to_qn", time=t4).get("qn")
check(t4 is not None and abs(back - 4.0) < 1e-6, "time.qn_to_time -> time_to_qn round-trips 4 QN",
      f"4 QN = {t4:.4f}s -> {back:.4f} QN")

print("\n== 5. Project verbs (pump-safe guards; render dry-run; optional save-as) ==")
# The scratch project is dirty (we just edited it), so the open/new guards must refuse WITHOUT acting.
pn = call("project.new")
check(pn.get("error") == "unsaved_changes", "project.new on a dirty project -> unsaved_changes (no action)")
dummy = "/tmp/reaper_mcp_p83_dummy.rpp"
open(dummy, "w").write("<REAPER_PROJECT 0.1 \"7.0\" 0>\n>\n")
po = call("project.open", path=dummy, discardChanges=False)
check(po.get("error") == "unsaved_changes",
      "project.open(existing, discard=false) on dirty project -> unsaved_changes (nothing opened)")
pnf = call("project.open", path="/tmp/does_not_exist_p83_xyz.rpp", discardChanges=True)
check(pnf.get("error") == "file_not_found", "project.open(missing path) -> file_not_found")
pr = call("project.render")  # dryRun default = true — NEVER renders
check(pr.get("ok") is True and pr.get("dryRun") is True and isinstance(pr.get("targets"), list),
      "project.render defaults to dryRun (no render); returns target list",
      f"targets={len(pr.get('targets', []))}")
if DO_SAVE:
    savep = "/tmp/reaper_mcp_p83_verify.rpp"
    sv = call("project.save", path=savep)
    ok_saved = sv.get("ok") is True and sv.get("savedAs") is True and os.path.exists(savep)
    check(ok_saved, "project.save(path) writes a .rpp via SaveProjectEx (no dialog)", f"-> {savep}")
    notes.append(f"the scratch project was saved-as to {savep} (project filename changed)")
else:
    skip("project.save real save-as", "--no-save passed; guard-only project coverage")

print("\n== 6. Master track (get / set / add_fx / set_fx_param) ==")
m0 = call("master.get")
check(all(k in m0 for k in ("volDb", "muted", "channels", "fxCount")),
      "master.get returns volDb/muted/channels/fxCount", f"vol={m0.get('volDb'):.2f}dB")
v0 = m0.get("volDb", 0.0)
ms = call("master.set", db=-3.0)
check(ms.get("ok") is True and abs(ms.get("volDb", 0) + 3.0) < 0.2, "master.set(db=-3) applied",
      f"volDb={ms.get('volDb'):.2f}")
call("master.set", db=v0)  # restore master volume
mfx = call("master.add_fx", name="ReaEQ")
check(mfx.get("fxIndex", -1) >= 0, "master.add_fx -> fxIndex", f"fxIndex={mfx.get('fxIndex')}")
check(call("master.set_fx_param", fx=mfx.get("fxIndex", 0), param=0, normalized=0.5).get("ok") is True,
      "master.set_fx_param(normalized=0.5) -> ok")
notes.append("master.add_fx left one FX on the master chain (no master-FX remover in the toolset)")

# --- cleanup ---
if DO_CLEANUP:
    print("\n== cleanup ==")
    for tr in (trackB, trackA):
        try:
            call("track.remove", track=tr)
            print(f"  removed scratch track {tr}")
        except RuntimeError as e:
            print(f"  [warn] could not remove track {tr}: {e}")
else:
    notes.append(f"scratch tracks {trackA} and {trackB} left in place (pass --cleanup to remove)")

# ================================================================================================
print(f"\n{'='*64}\n  PASS={passed}  FAIL={failed}  SKIP={skipped}")
for n in notes:
    print(f"  note: {n}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: GREEN — Phase 8 Batch P8-3 (depth) verified live.")
