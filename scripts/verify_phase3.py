#!/usr/bin/env python3
"""Live-verify Phase 3 (MCP resource subscriptions) against a running reaper_mcp.

Opens the SSE channel (GET /mcp), subscribes to resources, makes real edits, and asserts that
`notifications/resources/updated` is pushed for the changed + subscribed resources — and NOT for
unsubscribed ones. Reads the endpoint + bearer token from the extension's discovery file
(reaper_mcp.json). Stdlib only (urllib + threading) — no pip installs. macOS/Linux.

Usage:
    python3 verify_phase3.py                 # auto-find reaper_mcp.json
    python3 verify_phase3.py /path/to/reaper_mcp.json
    python3 verify_phase3.py --cleanup       # also delete the test tracks when done

It adds up to two tracks named "MCP phase3 test …". Everything is single-undo (Cmd/Ctrl+Z), or
pass --cleanup. Requires REAPER open with the extension loaded (run 'reaper_mcp: status' once).
"""
import json
import os
import sys
import threading
import time
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

# --- JSON-RPC over POST -------------------------------------------------------------------------
_id = 0
def rpc(method, params=None, want_error=False):
    global _id
    _id += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Content-Type": "application/json", "Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=10) as r:
        j = json.loads(r.read())
    if want_error:
        return j
    if "error" in j:
        raise RuntimeError(f"{method} -> {j['error']}")
    return j["result"]

def call(tool_name, **arguments):
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError(f"{tool_name} isError: {r.get('content')}")
    return r["structuredContent"]

# --- SSE reader (GET /mcp) on a background daemon thread ----------------------------------------
_sse_lock = threading.Lock()
_sse_events = []           # parsed JSON-RPC notifications seen on the stream
_sse_stop = threading.Event()
_sse_ready = threading.Event()

def _sse_reader():
    req = urllib.request.Request(URL, method="GET", headers={
        "Authorization": "Bearer " + TOKEN, "Accept": "text/event-stream"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            _sse_ready.set()
            for raw in r:                       # yields one line at a time as data arrives
                if _sse_stop.is_set():
                    break
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue                    # ": heartbeat" comments and blank separators
                try:
                    msg = json.loads(line[5:].strip())
                except ValueError:
                    continue
                with _sse_lock:
                    _sse_events.append(msg)
    except Exception as e:                       # daemon thread — swallow on shutdown/timeout
        if not _sse_stop.is_set():
            print(f"  (SSE reader stopped: {e})")

def updates_for(uri):
    with _sse_lock:
        return [m for m in _sse_events
                if m.get("method") == "notifications/resources/updated"
                and m.get("params", {}).get("uri") == uri]

def wait_update(uri, timeout=8.0, minimum=1):
    """Wait until at least `minimum` updated-notifications for `uri` have been seen."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(updates_for(uri)) >= minimum:
            return True
        time.sleep(0.05)
    return False

passed = failed = 0
def check(cond, label, detail=""):
    global passed, failed
    mark = "PASS" if cond else "FAIL"
    if cond: passed += 1
    else: failed += 1
    print(f"  [{mark}] {label}" + (f"  — {detail}" if detail else ""))

STATE = "reaper://project/state"
GRAPH = "reaper://routing/graph"

tracks = []
sse_thread = threading.Thread(target=_sse_reader, daemon=True)
try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"])
    check(info["capabilities"]["resources"].get("subscribe") is True,
          "initialize advertises resources.subscribe = true")

    sse_thread.start()
    check(_sse_ready.wait(timeout=5), "SSE channel (GET /mcp) opened")
    time.sleep(0.3)

    print("\n== subscribe validation ==")
    e1 = rpc("resources/subscribe", {}, want_error=True)
    check(e1.get("error", {}).get("code") == -32602, "subscribe without uri -> -32602")
    e2 = rpc("resources/subscribe", {"uri": "reaper://does/not/exist"}, want_error=True)
    check(e2.get("error", {}).get("code") == -32602, "subscribe to unknown resource -> -32602")

    print("\n== subscribe -> push on real edits ==")
    rpc("resources/subscribe", {"uri": STATE})
    rpc("resources/subscribe", {"uri": GRAPH})

    trk = call("track.add", name="MCP phase3 test A")["trackIndex"]
    tracks.append(trk)
    print(f"  (added track index = {trk} — fires SetTrackListChange)")
    check(wait_update(STATE), "project/state updated pushed after track.add")
    check(wait_update(GRAPH), "routing/graph updated pushed after track.add (topology change)")

    print("\n== unsubscribe stops delivery ==")
    rpc("resources/unsubscribe", {"uri": GRAPH})
    state_before = len(updates_for(STATE))
    graph_before = len(updates_for(GRAPH))
    trk2 = call("track.add", name="MCP phase3 test B")["trackIndex"]
    tracks.append(trk2)
    print(f"  (added track index = {trk2} after unsubscribing routing/graph)")
    check(wait_update(STATE, minimum=state_before + 1),
          "still-subscribed project/state gets a new update", f"count {state_before} -> {len(updates_for(STATE))}")
    time.sleep(0.6)  # give any stray notification time to arrive
    check(len(updates_for(GRAPH)) == graph_before,
          "unsubscribed routing/graph gets NO further updates", f"count stayed {graph_before}")

    print("\n== coalescing (informational) ==")
    # A track-scoped edit (fader) may fire SetSurfaceVolume; API-driven surface callbacks vary by
    # REAPER build, so this is a probe, not a gate. When it fires, the burst coalesces to one update.
    chunk = f"reaper://track/{trk}/chunk"
    rpc("resources/subscribe", {"uri": chunk})
    base = len(updates_for(chunk))
    for db in (-6.0, -3.0, 0.0):
        try: call("track.set_fader", track=trk, db=db)
        except Exception: pass
    fired = wait_update(chunk, timeout=2.0, minimum=base + 1)
    got = updates_for(chunk)
    print(f"  [{'PASS' if fired else 'note'}] track/{trk}/chunk update after fader moves "
          f"— {len(got)-base} coalesced notification(s)"
          + ("" if fired else " (surface didn't emit for API-driven fader change; not a gate)"))

finally:
    _sse_stop.set()
    if CLEANUP and tracks:
        for t in sorted(tracks, reverse=True):   # remove high indices first
            try:
                call("track.remove", track=t)
            except Exception as e:
                print(f"(cleanup: could not remove track {t}: {e})")
        print(f"\n(cleaned up test tracks {sorted(tracks)})")

print(f"\n{'='*48}\n{passed} passed, {failed} failed")
if not CLEANUP and tracks:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
