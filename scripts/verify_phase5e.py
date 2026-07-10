#!/usr/bin/env python3
"""Live-verify Phase 5 batch (e) — session.run_dsl, the deterministic macro-DSL runner.

Plain-REAPER gate — NO IEM / ambisonic suite required (folds into Session B or C). Reads the endpoint +
bearer token from the extension's discovery file (reaper_mcp.json), then drives session.run_dsl against a
running REAPER and asserts real behaviour + project state:

  1. tool surface = 61, and session.run_dsl is present (full profile);
  2. dryRun: a 3-line script mixing all three preview modes — a mutating primitive (build_bed, listed only),
     a dry-run composite (apply_style), and a read-only eval (check_deliverable) — returns a composed plan
     + diff, resolves each verb to the right tool, and MUTATES NOTHING (track count unchanged);
  3. apply: a real 2-line script (apply_style on a known track + a supplied-measurement check) runs both
     steps, composes their structured results, reports a single undo point, and the FX actually land;
  4. structured errors, each mutating nothing: an unknown verb -> dsl_parse_error; a fully-qualified but
     unregistered tool -> unknown_tool; a bad style caught by the non-mutating pre-flight ->
     dsl_validation_error (BEFORE any edit);
  5. ATOMIC ROLLBACK (the headline transactional guarantee — the one thing the cloud build can't prove):
     a script whose step 1 really mutates (apply_style inserts a chain) and whose step 2 throws at apply
     time (set_channels on a bogus index — a primitive, so it slips past pre-flight) is rolled back as ONE
     undo point => the step-1 FX are GONE afterwards (fx count back to baseline). The non-atomic variant
     (atomic=false) keeps the step-1 FX and reports rolledBack=false.

If REAPER coalesces nested undo blocks as expected, (5) leaves the rollback track at its baseline FX count;
if it does not, (5) fails loudly and tells you the undo model needs revisiting. REAPER must be in the
FOREGROUND — App Nap stalls the Run() pump. Stdlib only.

Usage:
    python3 verify_phase5e.py                 # auto-find reaper_mcp.json
    python3 verify_phase5e.py /path/to/reaper_mcp.json
    python3 verify_phase5e.py --cleanup       # also delete the test tracks when done
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

def run_dsl(script, **kw):
    return call("session.run_dsl", script=script, **kw)

passed = failed = skipped = 0
def check(cond, label, detail=""):
    global passed, failed
    mark = "PASS" if cond else "FAIL"
    if cond: passed += 1
    else: failed += 1
    print(f"  [{mark}] {label}" + (f"  — {detail}" if detail else ""))

def track_count():
    return len(call("track.list")["tracks"])

def fx_count(idx):
    return call("fx.list", track=idx)["fxCount"]

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
    check(len(tools) == 61, "tools/list returns 61 tools (60 + session.run_dsl)", f"got {len(tools)}")
    check("session.run_dsl" in names, "session.run_dsl present")

    # ---- 2. dryRun: composed plan across all three preview modes, mutates nothing -----------------
    print("\n== dryRun — composed plan (build_bed listed, apply_style dry-run, check read-only) ==")
    t0 = add_src("MCP p5e Dry")
    n_before = track_count()
    script = (
        '# a mixed script exercising the three preview modes\n'
        'build_bed layout=7.1.4 busName="MCP p5e (planned)"\n'
        f'apply_style track={t0} style=pop-bright\n'
        'check spec=atmos-music measured={"lufsIntegrated":-18,"truePeak":-1,"channels":12}'
    )
    d = run_dsl(script, dryRun=True)
    check(d.get("ok") is True and d.get("dryRun") is True, "dryRun -> ok + dryRun")
    check(d.get("stepCount") == 3 and len(d.get("steps", [])) == 3, "parsed + previewed 3 steps",
          f'stepCount={d.get("stepCount")}')
    steps = d.get("steps", [])
    tools_in_order = [s.get("tool") for s in steps]
    check(tools_in_order == ["spatial.build_bed", "mix.apply_style", "analysis.check_deliverable"],
          "verbs resolved to the right tools, in order", str(tools_in_order))
    check(steps[0]["result"].get("previewed") is False, "primitive (build_bed) is listed, not executed")
    check(steps[1].get("error") is False, "apply_style dry-run step is not an error")
    check(steps[2]["result"].get("pass") is True, "check_deliverable eval ran read-only (atmos -18/-1 pass)")
    check(isinstance(d.get("plan"), list) and "mix.apply_style" in d.get("diff", ""),
          "dryRun carries the structured plan + human diff")
    check(track_count() == n_before, "dryRun mutated NOTHING (no bed built)", f"{n_before} -> {track_count()}")
    check(fx_count(t0) == 0, "dryRun inserted no FX on the target track")

    # ---- 3. apply: real 2-line script -> composed results, one undo point -------------------------
    print("\n== apply — real 2-line script (apply_style + supplied-measurement check) ==")
    ta = add_src("MCP p5e Apply")
    base_a = fx_count(ta)
    ascript = (
        f'apply_style track={ta} style=pop-bright\n'
        'check spec=streaming-stereo measured={"lufsIntegrated":-14,"truePeak":-1,"channels":2}'
    )
    a = run_dsl(ascript)
    check(a.get("ok") is True, "apply -> ok")
    check(a.get("stepCount") == 2 and len(a.get("steps", [])) == 2, "both steps executed")
    check(a["steps"][1]["result"].get("pass") is True, "composed the check_deliverable pass/fail (-14 pass)")
    check("undo point" in a.get("undo", ""), "reports a single undo point", a.get("undo"))
    check(fx_count(ta) > base_a, "the style chain really landed on the track", f"{base_a} -> {fx_count(ta)}")

    # ---- 4. structured errors — each mutates nothing ---------------------------------------------
    print("\n== structured errors ==")
    n_err = track_count()
    pe = run_dsl("frobnicate x=1")
    check(pe.get("ok") is False and pe.get("error") == "dsl_parse_error", "unknown verb -> dsl_parse_error")
    check(pe.get("errors", [{}])[0].get("code") == "unknown_verb", "parse error carries the code")

    ut = run_dsl("spatial.does_not_exist track=0")
    check(ut.get("error") == "unknown_tool", "unregistered fully-qualified tool -> unknown_tool",
          str(ut.get("error")))

    ve = run_dsl(f"apply_style track={ta} style=totally-bogus")
    check(ve.get("error") == "dsl_validation_error", "bad style caught by pre-flight -> dsl_validation_error")
    check(ve.get("failedStep") == 1, "validation error points at the offending step")
    check(track_count() == n_err, "all three error paths mutated nothing", f"{n_err} -> {track_count()}")

    # ---- 5. ATOMIC ROLLBACK — the transactional guarantee ----------------------------------------
    print("\n== atomic rollback (step 1 mutates, step 2 throws -> whole run reverts) ==")
    tr = add_src("MCP p5e Rollback")
    base_r = fx_count(tr)
    # step 2 (set_channels on a bogus index) is a primitive -> it slips past the composite pre-flight and
    # only fails at apply time, which is exactly the case that must trigger the outer-undo rollback.
    rscript = (
        f'apply_style track={tr} style=pop-bright\n'
        'set_channels track=999999 channels=6'
    )
    ra = run_dsl(rscript)  # atomic defaults to true
    check(ra.get("ok") is False and ra.get("error") == "dsl_step_failed", "atomic run reports step failure")
    check(ra.get("failedStep") == 2, "failure attributed to step 2 (bad set_channels)")
    check(ra.get("rolledBack") is True, "run reports it rolled back", f'rolledBack={ra.get("rolledBack")}')
    check(fx_count(tr) == base_r,
          "ATOMIC: step-1 FX were reverted by the single undo (nested blocks coalesced)",
          f"{base_r} -> {fx_count(tr)}")

    # non-atomic variant: keep what applied, report how far it got.
    print("\n== non-atomic partial apply (atomic=false keeps the applied step) ==")
    tr2 = add_src("MCP p5e Partial")
    base_r2 = fx_count(tr2)
    rp = run_dsl(f'apply_style track={tr2} style=pop-bright\nset_channels track=999999 channels=6',
                 atomic=False)
    check(rp.get("ok") is False and rp.get("rolledBack") is False, "non-atomic reports failure without rollback")
    check(fx_count(tr2) > base_r2, "non-atomic KEPT the step-1 FX (partial apply)", f"{base_r2} -> {fx_count(tr2)}")

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
