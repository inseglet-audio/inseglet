#!/usr/bin/env python3
"""Live-verify Phase 2 batch 2 (takes/MIDI + resources) against a running reaper_mcp.

Reads the endpoint + bearer token from the extension's discovery file (reaper_mcp.json),
then drives initialize -> tools/list -> MIDI take round-trip -> resources/read over HTTP.
Stdlib only (urllib) — no pip installs. macOS/Linux.

Usage:
    python3 verify_batch2.py                 # auto-find reaper_mcp.json
    python3 verify_batch2.py /path/to/reaper_mcp.json
    python3 verify_batch2.py --cleanup       # also delete the test track when done

It adds one track named "MCP batch2 test" with a short MIDI item so you can eyeball the
notes in REAPER's MIDI editor. Everything is single-undo (Cmd/Ctrl+Z), or pass --cleanup.
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

trk = None
try:
    info = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
    print("server   :", info["serverInfo"], "\n")

    tools = rpc("tools/list")["tools"]
    names = {t["name"] for t in tools}
    print("== tool surface ==")
    check(len(tools) == 47, "tools/list returns 47 tools", f"got {len(tools)}")
    for want in ["midi.create_item", "midi.insert_note", "midi.list_notes", "midi.insert_cc",
                 "take.list", "take.set_active"]:
        check(want in names, f"{want} present")

    print("\n== takes + MIDI round-trip ==")
    trk = call("track.add", name="MCP batch2 test")["trackIndex"]
    print(f"  (test track index = {trk})")
    item = call("midi.create_item", track=trk, position=0, length=4)["item"]
    check(isinstance(item, int), "midi.create_item returns an item index", f"item={item}")

    n1 = call("midi.insert_note", track=trk, item=item, startBeats=0, lengthBeats=1, pitch=60, velocity=100)
    n2 = call("midi.insert_note", track=trk, item=item, startBeats=1, lengthBeats=0.5, pitch=64, velocity=90)
    check(n1["noteCount"] == 1 and n2["noteCount"] == 2, "insert_note note count grows 1 -> 2",
          f'{n1["noteCount"]} then {n2["noteCount"]}')

    cc = call("midi.insert_cc", track=trk, item=item, beats=0, messageType="cc", cc=7, value=100)
    check(cc["ccCount"] == 1, "insert_cc (CC7=100) lands", f'ccCount={cc["ccCount"]}')

    ln = call("midi.list_notes", track=trk, item=item)
    beats = [(round(n["startBeats"], 3), round(n["lengthBeats"], 3), n["pitch"]) for n in ln["notes"]]
    check(ln["noteCount"] == 2, "list_notes sees 2 notes", str(beats))
    check(any(abs(n["startBeats"]) < 1e-6 and n["pitch"] == 60 for n in ln["notes"])
          and any(abs(n["startBeats"] - 1) < 1e-6 and n["pitch"] == 64 for n in ln["notes"]),
          "beats round-trip exactly (0/C4, 1/E4)")

    lc = call("midi.list_cc", track=trk, item=item)
    check(lc["ccCount"] == 1 and lc["events"][0]["messageType"] == "cc" and lc["events"][0]["value"] == 100,
          "list_cc decodes CC7=100", str(lc["events"]))

    tk = call("take.list", track=trk, item=item)
    check(tk["takeCount"] == 1 and tk["takes"][0]["isMidi"], "take.list: 1 MIDI take", str(tk["takes"]))

    print("\n== resources ==")
    res = {r["uri"] for r in rpc("resources/list")["resources"]}
    check("reaper://project/state" in res and "reaper://routing/graph" in res,
          "resources/list has project/state + routing/graph", str(sorted(res)))
    tmpl = {r["uriTemplate"] for r in rpc("resources/templates/list")["resourceTemplates"]}
    check("reaper://track/{index}/chunk" in tmpl, "templates/list has track chunk template")

    rr = rpc("resources/read", {"uri": "reaper://routing/graph"})["contents"][0]
    graph = json.loads(rr["text"])
    check(rr["mimeType"] == "application/json" and "nodes" in graph and "edges" in graph,
          "routing/graph reads as JSON {nodes,edges}",
          f'{len(graph["nodes"])} nodes, {len(graph["edges"])} edges, master {graph["master"]}')

    cr = rpc("resources/read", {"uri": f"reaper://track/{trk}/chunk"})["contents"][0]
    check(cr["mimeType"] == "text/plain" and cr["text"].lstrip().startswith("<TRACK"),
          "track/{index}/chunk returns raw .RPP text", f'{len(cr["text"])} bytes, starts {cr["text"].splitlines()[0]!r}')

    pr = json.loads(rpc("resources/read", {"uri": "reaper://project/state"})["contents"][0]["text"])
    check(pr["trackCount"] >= 1, "project/state snapshot", f'name={pr["name"]!r}, {pr["trackCount"]} tracks, {pr["tempo"]} bpm')

finally:
    if CLEANUP and trk is not None:
        try:
            call("track.remove", track=trk)
            print(f"\n(cleaned up test track {trk})")
        except Exception as e:
            print(f"\n(cleanup failed: {e} — remove the 'MCP batch2 test' track manually)")

print(f"\n{'='*48}\n{passed} passed, {failed} failed")
if not CLEANUP and trk is not None:
    print("Test edits left in the project — Cmd/Ctrl+Z to undo, or re-run with --cleanup.")
sys.exit(1 if failed else 0)
