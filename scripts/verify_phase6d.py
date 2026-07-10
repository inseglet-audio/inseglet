#!/usr/bin/env python3
"""Live-verify Phase 6 batch (d) — full MCP elicitation (Option A, spec-faithful async).

Session P6-B gate. THIS SCRIPT IS THE ELICITATION-CAPABLE CLIENT: it advertises
`capabilities.elicitation` at initialize, then drives the destructive verb `spatial.setup_immersive_session`
through the real async round-trip against a running REAPER + the loaded extension, asserting project state:

  0. surface = 61 (unchanged — elicitation is transport, adds no tool).
  1. ACCEPT — on a NON-EMPTY project the verb answers its tools/call POST with an SSE stream carrying a
     server->client `elicitation/create`; this client POSTs an accept on a second connection; the server
     resumes, RE-MARSHALS the mutation to the main thread (confirm folded in) and streams the tools/call
     result. Assert: the stream carried elicitation/create; the result built the bed (bedBus present);
     the project gained tracks.
  2. DECLINE — same call, this client declines; the result is an isError ("declined") and the project
     track count is UNCHANGED (zero mutation).
  3. FALLBACK — re-initialize WITHOUT elicitation; the verb (no confirm) returns the confirm:true stopgap
     (`confirmation_required`) as a normal JSON body — no SSE, no elicitation.

The 120 s server-side timeout path is proven in the cloud protocol test (with a short cap) — not re-run
here (it would idle 2 min). REAPER must be in the FOREGROUND (App Nap stalls the Run() pump). Stdlib only.

Usage:
    python3 verify_phase6d.py                  # auto-find reaper_mcp.json
    python3 verify_phase6d.py /path/to/reaper_mcp.json
    python3 verify_phase6d.py --cleanup        # also delete the tracks this gate created
"""
import http.client
import json
import os
import sys
import urllib.error
import urllib.request
from urllib.parse import urlparse

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
    """Synchronous tools/call -> structuredContent (a recoverable {error,...} is returned as data)."""
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r.get("structuredContent", {})

def initialize(elicit):
    caps = {"elicitation": {}} if elicit else {}
    return rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": caps,
                              "clientInfo": {"name": "verify_phase6d", "version": "0"}})

def post_answer(elic_id, action):
    """POST the client's elicitation response on its own connection (server replies 202, no body)."""
    body = json.dumps({"jsonrpc": "2.0", "id": elic_id,
                       "result": {"action": action, "content": {"confirm": action == "accept"}}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status  # 202

def elicited_call(tool_name, arguments, action):
    """Drive a tools/call that may elicit. If the server answers with an SSE stream, read the
    elicitation/create, POST `action`, then read the streamed tools/call result. Returns
    (result_obj, elicited: bool). If the server answers synchronously (no elicitation), returns its
    JSON-RPC result with elicited=False."""
    cid = next_id()
    body = json.dumps({"jsonrpc": "2.0", "id": cid, "method": "tools/call",
                       "params": {"name": tool_name, "arguments": arguments}})
    conn = http.client.HTTPConnection(U.hostname, U.port, timeout=20)
    conn.request("POST", U.path, body.encode(), HEADERS)
    resp = conn.getresponse()
    if "text/event-stream" not in (resp.getheader("Content-Type") or ""):
        j = json.loads(resp.read() or b"{}")
        conn.close()
        return j.get("result"), False
    elicited = False
    result = None
    while True:
        raw = resp.readline()
        if not raw:
            break
        line = raw.decode("utf-8", "replace").strip()
        if not line.startswith("data:"):
            continue  # SSE comment / heartbeat / blank
        msg = json.loads(line[len("data:"):].strip())
        if msg.get("method") == "elicitation/create":
            elicited = True
            post_answer(msg["id"], action)
        elif "result" in msg and msg.get("id") == cid:
            result = msg["result"]
            break
    conn.close()
    return result, elicited

passed = failed = skipped = 0
def check(cond, label, detail=""):
    global passed, failed
    if cond: passed += 1
    else: failed += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}" + (f"  — {detail}" if detail else ""))

def track_count():
    return call("track.list").get("trackCount", 0)

def server_has_elicitation_lane():
    """Distinguish a p6d build from an older one WITHOUT mutating anything: post a stray JSON-RPC
    RESPONSE (id, no method, a result). The p6d server routes it to the elicitation correlator and
    acks 202; an older server has no such route and replies 200 with a JSON-RPC error. Returns True
    iff the loaded extension has the batch-(d) transport."""
    body = json.dumps({"jsonrpc": "2.0", "id": "__probe__", "result": {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status == 202
    except urllib.error.HTTPError as e:
        return e.code == 202

# ------------------------------------------------------------------------------------------------
created_names = {"p6d-scratch", "5.1 Bed"}  # for --cleanup

# Fail fast + loud if the running extension predates batch (d) — otherwise the elicitation checks all
# read like the confirm:true fallback (no SSE, confirmation_required for accept), which is confusing.
if not server_has_elicitation_lane():
    print("  [FATAL] The loaded reaper_mcp extension has NO elicitation lane — it predates batch (d).")
    print("          (a stray JSON-RPC response was not accepted with 202.)")
    print("          You are running against a stale build. Rebuild, re-sign, and RELAUNCH REAPER:\n")
    print("            cmake --build build --config Release")
    print("            codesign -s - --force build/reaper_mcp.dylib")
    print("            # then Cmd-Q REAPER, relaunch (keep it FOREGROUND), re-run this gate\n")
    sys.exit(2)

print("== 0. surface + capability ==")
initialize(True)
enum = call("tools.enumerate", profile="all")
check(enum.get("count") == 61, "tool surface == 61 (elicitation adds no tool)", f"count={enum.get('count')}")

print("\n== ensure a non-empty project (elicitation only gates a non-empty project) ==")
call("track.add", name="p6d-scratch")
base = track_count()
check(base > 0, "project is non-empty", f"trackCount={base}")

print("\n== 1. ACCEPT — elicitation round-trip builds the bed ==")
res, elicited = elicited_call("spatial.setup_immersive_session", {"bed": "5.1", "objectCount": 0}, "accept")
check(elicited, "server streamed elicitation/create on the tools/call SSE response")
check(res is not None and not res.get("isError", True), "accepted call returned a non-error result")
sc = (res or {}).get("structuredContent", {})
check(sc.get("ok") is True and "bedBus" in sc, "the mutation ran after accept (bed built, bedBus present)",
      f"bedBus={sc.get('bedBus')}")
after_accept = track_count()
check(after_accept > base, "project gained tracks (the bed bus)", f"{base} -> {after_accept}")

print("\n== 2. DECLINE — no mutation ==")
before_decline = track_count()
res2, elicited2 = elicited_call("spatial.setup_immersive_session", {"bed": "5.1", "objectCount": 0}, "decline")
check(elicited2, "server streamed elicitation/create again")
check(res2 is not None and res2.get("isError") is True, "declined call is an isError result")
txt = (res2 or {}).get("content", [{}])[0].get("text", "") if res2 else ""
check("declin" in txt.lower(), "result message says the user declined", txt[:80])
check(track_count() == before_decline, "declining mutated NOTHING (track count unchanged)",
      f"{before_decline} == {track_count()}")

print("\n== 3. FALLBACK — a client without elicitation gets the confirm:true stopgap ==")
initialize(False)
res3, elicited3 = elicited_call("spatial.setup_immersive_session", {"bed": "5.1", "objectCount": 0}, "accept")
check(not elicited3, "no elicitation stream when the client did not advertise it")
sc3 = (res3 or {}).get("structuredContent", {})
check(sc3.get("error") == "confirmation_required", "non-elicitation client falls back to confirmation_required",
      sc3.get("error", ""))

if CLEANUP:
    print("\n== cleanup — remove the tracks this gate created (by name; highest index first) ==")
    tl = call("track.list")
    victims = sorted((t.get("index") for t in tl.get("tracks", []) if t.get("name") in created_names),
                     reverse=True)
    for idx in victims:
        try:
            call("track.remove", track=idx)  # track.remove's arg is 'track' (an index), not 'index'
            print(f"  removed track {idx}")
        except Exception as e:  # noqa
            print(f"  (could not remove track {idx}: {e})")

print(f"\n{'='*60}\n  PASS={passed}  FAIL={failed}  SKIP={skipped}")
if failed:
    print("  RESULT: FAIL")
    sys.exit(1)
print("  RESULT: GREEN — Phase 6 (d) elicitation verified live. Phase 6 complete.")
