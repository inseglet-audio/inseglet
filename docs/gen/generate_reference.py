#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 James Livingston
"""Render docs/REFERENCE.md from the live registry dump produced by docs/gen/dump_reference.cpp.

Usage:  generate_reference.py [registry_dump.json] [docs/REFERENCE.md]

Normally invoked via the `reference-doc` CMake target:
    cmake --build build --target reference-doc

The dump is emitted by dump_reference.cpp linked against reaper_mcp_hostcore, so this document is
generated from the SAME registry the running server serves over tools/list, resources/list, and
prompts/list — it is not hand-maintained.
"""
import datetime
import json
import sys

IN_PATH = sys.argv[1] if len(sys.argv) > 1 else "registry_dump.json"
OUT_PATH = sys.argv[2] if len(sys.argv) > 2 else "docs/REFERENCE.md"
GEN_DATE = datetime.date.today().isoformat()
PROTOCOL = "2025-06-18"
REGEN_CMD = "cmake --build build --target reference-doc"

d = json.load(open(IN_PATH))
tools = d["tools"]
resources = d["resources"]
prompts = d["prompts"]
tool_count = d["toolCount"]

# Profile display order + human labels.
PROFILE_ORDER = ["core", "spatial", "mixing", "routing", "midi", "render", "analysis", "full"]
PROFILE_LABEL = {
    "core":     ("Core", "Transport, project, tracks, markers, items, tempo, time selection, and the always-on `tools.enumerate` meta-tool."),
    "spatial":  ("Spatial / immersive", "Immersive/spatial audio: channel beds, ambisonic encode/decode, surround panners, scene rotation, binaural monitoring, and live head-tracking."),
    "mixing":   ("Mixing", "Track FX, parameter automation envelopes, faders, and immersive-aware style chains."),
    "routing":  ("Routing", "Track-to-track sends and channel-count management."),
    "midi":     ("MIDI", "Takes and MIDI note / CC CRUD."),
    "render":   ("Render", "Multichannel / immersive deliverable rendering."),
    "analysis": ("Analysis", "Deliverable-spec conformance (loudness + true-peak, per-bed)."),
    "full":     ("Composite / DSL", "The deterministic composite macro-DSL runner (`$ref`/capture, atomic single-undo)."),
}

def esc(s):
    return str(s).replace("|", "\\|").replace("\n", " ").strip()

def type_of(prop):
    if "enum" in prop:
        return "enum"
    t = prop.get("type")
    if isinstance(t, list):
        return " \\| ".join(t)
    if t == "array":
        it = prop.get("items", {})
        it_t = it.get("type", "object") if isinstance(it, dict) else "object"
        return f"array&lt;{it_t}&gt;"
    return t or ("object" if "properties" in prop else "—")

def notes_of(prop):
    bits = []
    if "enum" in prop:
        vals = ", ".join(f"`{v}`" for v in prop["enum"])
        bits.append(f"one of: {vals}")
    if "default" in prop:
        bits.append(f"default `{json.dumps(prop['default'])}`")
    if "minimum" in prop and "maximum" in prop:
        bits.append(f"range [{prop['minimum']}, {prop['maximum']}]")
    elif "minimum" in prop:
        bits.append(f"min {prop['minimum']}")
    elif "maximum" in prop:
        bits.append(f"max {prop['maximum']}")
    if "description" in prop:
        bits.append(esc(prop["description"]))
    if prop.get("type") == "object" or "properties" in prop:
        if not bits:
            bits.append("object — see full schema below")
    return "; ".join(bits) or "—"

def hints(t):
    a = t["annotations"]
    tags = []
    if a.get("readOnlyHint"): tags.append("read-only")
    if a.get("destructiveHint"): tags.append("**destructive**")
    if a.get("idempotentHint"): tags.append("idempotent")
    if not tags:
        tags.append("mutating")
    return ", ".join(tags)

def param_table(schema):
    props = schema.get("properties", {})
    if not props:
        return "_No parameters._\n"
    req = set(schema.get("required", []))
    add = schema.get("additionalProperties", None)
    out = ["| Param | Type | Required | Notes |", "| --- | --- | --- | --- |"]
    for name, prop in props.items():
        prop = prop if isinstance(prop, dict) else {}
        out.append(f"| `{name}` | {type_of(prop)} | {'yes' if name in req else 'no'} | {notes_of(prop)} |")
    tbl = "\n".join(out) + "\n"
    if add is False:
        tbl += "\n_Additional properties: not allowed._\n"
    return tbl

def returns_summary(schema):
    if not schema:
        return "_No declared output schema._\n"
    props = schema.get("properties", {})
    if not props:
        return "_Structured output (see schema below)._\n"
    fields = ", ".join(f"`{k}`" for k in props.keys())
    return "Returns a structured object with: " + fields + ".\n"

lines = []
w = lines.append

w(f"# REAPER MCP — Tool, Resource & Prompt Reference\n")
w(f"> **Generated** {GEN_DATE} from the live in-process registry "
  f"(`docs/gen/dump_reference.cpp` linked against `reaper_mcp_hostcore`). "
  f"This mirrors exactly what the server serves over `tools/list`, `resources/list`, and `prompts/list` — "
  f"it is not hand-maintained. Regenerate after any surface change with `{REGEN_CMD}`.\n")
w("")
w(f"**Protocol:** MCP `{PROTOCOL}` · **Surface:** {tool_count} tools · "
  f"{len(resources)} resources · {len(prompts)} prompts.\n")
w("")
w("Tools are grouped by capability **profile**. Clients may negotiate a bounded profile set at "
  "`initialize` (to stay under LLM tool-count caps); `Profile::Full` (the default) exposes every tool. "
  "The always-on `tools.enumerate` meta-tool is visible under any profile.\n")

# Surface-at-a-glance counts
w("## Surface at a glance\n")
w("| Profile | Tools | Scope |")
w("| --- | --- | --- |")
by_prof = {}
for t in tools:
    by_prof.setdefault(t["profile"], []).append(t)
for p in PROFILE_ORDER:
    if p not in by_prof:
        continue
    label, scope = PROFILE_LABEL[p]
    w(f"| **{label}** (`{p}`) | {len(by_prof[p])} | {scope} |")
w(f"| **Total** | **{tool_count}** | |")
w("")

# TOC
w("## Contents\n")
w("- [Tools](#tools)")
for p in PROFILE_ORDER:
    if p in by_prof:
        label = PROFILE_LABEL[p][0]
        anchor = label.lower().replace(" / ", "--").replace(" ", "-")
        w(f"  - [{label}](#{anchor})")
w("- [Resources](#resources)")
w("- [Prompts](#prompts)")
w("")

# TOOLS
w("## Tools\n")
for p in PROFILE_ORDER:
    if p not in by_prof:
        continue
    label, scope = PROFILE_LABEL[p]
    w(f"### {label}\n")
    w(f"_{scope}_\n")
    for t in sorted(by_prof[p], key=lambda x: x["name"]):
        w(f"#### `{t['name']}`\n")
        meta = f"**Profile:** `{t['profile']}` · **Hints:** {hints(t)}"
        if t.get("alwaysVisible"):
            meta += " · **always visible**"
        w(meta + "\n")
        w(esc(t["description"]) + "\n")
        w("**Parameters**\n")
        w(param_table(t["inputSchema"]))
        if t.get("outputSchema"):
            w("**Returns**\n")
            w(returns_summary(t["outputSchema"]))
        w("<details><summary>Full JSON schema</summary>\n")
        w("```json")
        block = {"inputSchema": t["inputSchema"]}
        if t.get("outputSchema"):
            block["outputSchema"] = t["outputSchema"]
        w(json.dumps(block, indent=2))
        w("```")
        w("</details>\n")

# RESOURCES
w("## Resources\n")
w("Read-only, addressable snapshots of REAPER state (`resources/list`, `resources/read`, "
  "`resources/templates/list`). Templates carry `{placeholders}` resolved at read time. "
  "The project-state and routing-graph resources support subscriptions (`resources/subscribe`); "
  "the SSE channel flushes `notifications/resources/updated` when they change.\n")
w("| URI | Name | Type | MIME | Description |")
w("| --- | --- | --- | --- | --- |")
for r in resources:
    kind = "template" if r.get("isTemplate") else "static"
    w(f"| `{r['uri']}` | {esc(r['name'])} | {kind} | `{r['mimeType']}` | {esc(r['description'])} |")
w("")

# PROMPTS
w("## Prompts\n")
w("Declarative expert workflows (`prompts/list`, `prompts/get`). Selecting one injects a message that "
  "steers the agent to call the semantic verbs in the right order — an expert immersive workflow without "
  "knowing the verb names.\n")
for pr in prompts:
    title = pr.get("title") or pr["name"]
    w(f"### `{pr['name']}` — {esc(title)}\n")
    w(esc(pr["description"]) + "\n")
    args = pr.get("arguments", [])
    if args:
        w("| Argument | Required | Description |")
        w("| --- | --- | --- |")
        for a in args:
            w(f"| `{a['name']}` | {'yes' if a.get('required') else 'no'} | {esc(a.get('description',''))} |")
        w("")
    else:
        w("_No arguments._\n")

w("---\n")
w(f"_Reference generated {GEN_DATE} from the live registry (`{REGEN_CMD}`). "
  f"See `docs/CONVENTIONS.md` for channel-order, coordinate, bed-layout, and loudness-spec conventions, "
  f"and `SECURITY.md` for the transport threat model._\n")

with open(OUT_PATH, "w") as f:
    f.write("\n".join(lines))
print(f"Wrote {OUT_PATH}: {tool_count} tools, {len(resources)} resources, {len(prompts)} prompts.")
