% Inseglet — User Manual
% A native REAPER extension exposing a Model Context Protocol interface
% Version 1.5.0 · 2026

---

Inseglet is a native REAPER extension that lets a Model Context Protocol (MCP) client — such as an
AI assistant or any MCP-capable tool — drive REAPER directly: edit and mix a session, build spatial
scenes, meter immersive masters, and author object/bed deliverables. It runs *inside* REAPER as a
compiled extension, so control is real-time and event-driven.

This manual covers installation, connecting a client, the concepts you need, a catalog of what the
server can do, worked examples, and troubleshooting. For the exhaustive, machine-generated schema of
every tool, resource, and prompt, see **`docs/REFERENCE.md`** (regenerated from the live registry).

> This document is written for v1.5.0. Inseglet currently targets **macOS first**;
> Windows and Linux build in CI and follow once load-verified in a real REAPER.

---

## 1. Overview

Inseglet exposes **186 tools**, plus MCP **resources** (live project state you can read and subscribe
to) and expert **prompts**, over a local HTTP endpoint that speaks the MCP `2025-06-18` protocol. The
tools span everyday DAW work and a deep immersive/spatial layer:

- **Editing & mixing** — transport, tracks, items, markers/regions, FX, sends/routing, automation
  envelopes, tempo/time, MIDI.
- **Spatial / immersive** — channel beds (5.1 → 22.2), ambisonics (encode/decode, ACN/SN3D up to 7th
  order), scene rotation/mirroring, surround panners, binaural monitoring, live head-tracking.
- **Immersive metering** — gated loudness (program/stem/dialog/object), true-peak, ambisonic
  spatial-field analysis, downmix/binaural checks, decode coverage, loudness timelines.
- **Object / Atmos deliverables** — author an ITU-R BS.2076 ADM Broadcast-Wave, with a conformance mode
  shaped to the published Dolby Atmos Master ADM Profile, and a validator for any ADM BWF.
- **Semantic / agentic helpers** — named deliverable specs, immersive-aware style chains, and a
  deterministic composite macro-DSL for multi-step, single-undo operations.

Inseglet is MIT-licensed and community-developed. It is not affiliated with Cockos or Dolby (see
`TRADEMARKS.md`).

---

## 2. Requirements

- **REAPER** (a recent version) for your platform.
- **macOS** for the supported preview build (universal `x86_64;arm64`). Windows/Linux build in CI.
- To build from source: **CMake ≥ 3.20** and a C++17 compiler — AppleClang on macOS, MSVC on Windows,
  GCC/Clang on Linux.
- An **MCP client** to drive the server (for example an MCP-capable assistant, or the MCP Inspector for
  manual testing).

You do **not** need Python, a separate server process, or any network access beyond localhost — the
server runs in-process inside REAPER and listens only on the loopback interface.

---

## 3. Installation

### Option A — build from source

```bash
git clone https://github.com/inseglet-audio/inseglet.git && cd inseglet
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release -R unit      # optional: host-core unit + protocol suite (no REAPER)
cmake --install build                          # deploys the extension and the bundled JSFX
```

The build fetches the REAPER SDK, WDL, nlohmann/json, and cpp-httplib headers automatically. It produces
the platform extension binary and installs it into REAPER's `UserPlugins` folder:

| Platform | Artifact | Install path |
|---|---|---|
| macOS | `reaper_mcp.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| Windows | `reaper_mcp.dll` | `%APPDATA%\REAPER\UserPlugins\` |
| Linux | `reaper_mcp.so` | `~/.config/REAPER/UserPlugins/` |

> **Always run `cmake --install` after every rebuild.** Building alone leaves the previously installed
> binary in place; the install step copies the new binary into the folder REAPER actually loads from and
> ships the bundled JSFX effects to `Effects/MCP/`.

### Option B — ReaPack

A ReaPack index is provided under `packaging/reapack/`. Once the repository is listed, you can import the
index URL in ReaPack (Extensions → ReaPack → Import a repository) and install Inseglet like any other
package. (Confirm the published index URL in the release notes.)

After installing by either method, **restart REAPER** so it loads the extension.

---

## 4. First run

When REAPER starts with Inseglet installed, the extension starts its in-process MCP server and:

1. Prints its **loopback URL and bearer token** to the REAPER console
   (`View → Show console output` / the ReaScript console), e.g.
   `http://127.0.0.1:<port>/mcp`.
2. Writes a **discovery file** so clients can find the port and token without copy-paste:
   `~/Library/Application Support/REAPER/reaper_mcp.json` (macOS; the equivalent REAPER resource path on
   Windows/Linux). It contains the URL, port, bearer token, protocol version, and server name/version.

> **Keep REAPER in the foreground while a client is connected.** On macOS, App Nap suspends the UI timer
> when REAPER is in the background, which stalls the main-thread pump the server uses to apply commands.
> If commands seem to hang, bring REAPER to the front.

### The zero-install fallback

Before you install the binary, you can try a reduced toolset via the **bootstrap** ReaScript
(`bootstrap/`), which relays through REAPER's built-in web server. It exposes a subset of the surface and
is handy for a first look or as an independent test oracle; the compiled extension is the full experience.

---

## 5. Connecting an MCP client

Inseglet serves MCP over **Streamable HTTP + SSE** at:

```
http://127.0.0.1:<port>/mcp
```

Authentication is a **bearer token** (the one printed to the console / stored in the discovery file):

```
Authorization: Bearer <token>
```

### Generic client

Point your client at the endpoint above with the bearer token as an `Authorization` header. The server
implements the standard MCP lifecycle: `initialize` → `tools/list` → `tools/call`, plus `resources/*` and
`prompts/*`. It also supports `stdio` for clients that prefer a pipe transport.

### MCP Inspector

The MCP Inspector is the quickest way to explore interactively: connect it to the URL with the bearer
token, then browse `tools/list`, read resources, and call tools by hand. This is the recommended way to
learn the surface.

### Assistant clients (e.g. Claude Desktop)

Configure the assistant's MCP settings to reach the HTTP endpoint with the bearer header (using a bridge
such as `mcp-remote` if the client expects a command transport). Once connected, the assistant can call
Inseglet's tools directly.

### Capability profiles (staying under tool-count caps)

Many LLM clients cap how many tools they can hold at once. Inseglet groups its tools into **profiles**
(`core`, `spatial`, `mixing`, `routing`, `midi`, `render`, `analysis`, `full`). A client can negotiate a
bounded set of profiles at `initialize` to expose only what it needs; `Full` (the default) exposes
everything. The always-on **`tools.enumerate`** meta-tool is visible under any profile, so a client can
always discover what's available.

---

## 6. Core concepts

**Tools, resources, prompts.** *Tools* are callable actions (`track.add`, `spatial.export_adm`, …).
*Resources* are readable, subscribable state: `reaper://project/state`, `reaper://routing/graph`, and a
`reaper://track/{index}/chunk` template — subscribe to get change notifications over SSE instead of
polling. *Prompts* are expert workflow templates the server offers to clients.

**Single-undo mutations.** Every state change is wrapped in one REAPER undo block, so any tool call is a
single Ctrl/Cmd-Z away from being reverted.

**Dry-run.** Many mutating tools accept `dryRun: true` — they resolve and policy-check the operation and
report what *would* happen without changing anything. Use it to preview.

**Confirmation for destructive verbs (elicitation).** A few high-impact verbs (for example
`spatial.setup_immersive_session` on a non-empty project, or `track.set_state_chunk`) require explicit
confirmation. If your client supports MCP **elicitation**, the server asks mid-call and waits for your
answer without freezing REAPER; if it doesn't, pass `confirm: true` to authorize the operation.

**Action safety.** Running arbitrary REAPER actions by command ID is gated by an allow/deny policy so that
known modal or fatal actions can't be triggered blindly (see `SECURITY.md` and the `action.*` tools).

---

## 7. What Inseglet can do (tool catalog)

The full, machine-generated reference — every parameter and output schema — is in **`docs/REFERENCE.md`**.
The summary below orients you by area. Counts are for the v1.5.0 surface (186 tools total).

| Area (profile) | Tools | What's in it |
|---|---|---|
| **Core** (`core`) | 82 | Transport (play/stop/record/goto, repeat, play-rate, metronome), project summary, tracks (add/remove/select/name/fader/mute/solo/color/channels), items (add/split/glue/move/duplicate/fades), takes & comping (add/delete/crop, per-take volume/pan/pitch/play-rate, implode-to-takes), markers & regions, tempo & time-signature map, time selection, track state-chunk read/patch/write, freeze/unfreeze, peak metering, and the always-on `tools.enumerate`. |
| **Spatial / immersive** (`spatial`) | 23 | Channel beds 5.1 → 22.2, ambisonic encode/decode (ACN/SN3D, order conversion, format conversion), scene rotation & mirroring, beamforming, source distance/room, surround panners, one-call immersive/Atmos session scaffolding, object/bed authoring, idempotent object send-layout orchestration into the external Dolby Atmos Renderer, binaural monitoring, live OSC head-tracking, and native ADM export + inspection. |
| **Mixing** (`mixing`) | 27 | Track & take FX (list/add/remove/enable/preset, parameter get/set), automation envelopes (points, ranges, automation mode), faders, master FX, and immersive-aware style chains with a bundled multichannel true-peak limiter. |
| **Routing** (`routing`) | 8 | Track-to-track sends and receives (add/list/remove/set level & channels) and channel-count management. |
| **MIDI** (`midi`) | 23 | Takes plus MIDI note and CC create/read/update/delete, batch note insert, selection filters, and groove/quantization — quantize (grid + swing), humanize, groove-template extract & apply, transpose, velocity scaling, legato, nudge, and time-stretch. |
| **Render** (`render`) | 4 | Multichannel / immersive deliverable rendering, plus native ADM BWF and Dolby Atmos Master File (DAMF) triad master authoring. |
| **Analysis** (`analysis`) | 18 | Gated loudness (program, stem, dialog, per-object), true-peak, ambisonic spatial-field analysis, downmix & binaural QC, decode coverage, loudness timelines, named deliverable-spec conformance, ADM inspection / profile conformance checking, DAMF (Dolby Atmos Master File) triad inspection, renderer-bus send-layout inspection (roster QC), and render-free direct sample reads / accessor metering / frame-accurate silence detection via a REAPER audio accessor, and per-object decode-coverage timelines that track how each immersive object migrates across the delivery loudspeakers over time. |
| **Composite / DSL** (`full`) | 1 | `session.run_dsl` — a deterministic composite macro-DSL with `$ref`/capture and atomic single-undo, for multi-step operations. |

Use `tools.enumerate` (or `tools/list`) at runtime to get the authoritative, current list from your
installed build.

---

## 8. Worked examples

These sketches show the intent and the tools involved; consult `docs/REFERENCE.md` for exact parameters,
and remember you can add `dryRun: true` to preview any mutation.

### 8.1 Set up an immersive (Atmos) session

Use `spatial.setup_immersive_session` to scaffold a bed + object topology in one confirmed operation, or
compose it yourself: create a 7.1.2 bed, add object tracks with `spatial.author_object_bed`, and add
binaural monitoring with `spatial.add_binaural_monitor`. Because the scaffold is a destructive verb on a
non-empty project, your client will either prompt you (elicitation) or expect `confirm: true`.

### 8.2 Meter a master and check a deliverable

- `analysis.meter` — program loudness (integrated LUFS, short-term/momentary, LRA, true-peak) plus
  per-channel RMS/peak/dBTP with SMPTE bed labels and L/R correlation.
- `analysis.spatial_field` — for an ambisonic bus: direction of arrival, diffuseness, and the Gerzon
  velocity/energy vectors.
- `analysis.check_deliverable` — evaluate the master (or a named bus) against a named loudness/true-peak
  deliverable spec, with per-bed sub-metering.

### 8.3 Author and validate an ADM master

- `spatial.export_adm` — render the bed and object stems and author an ITU-R BS.2076 ADM Broadcast-Wave
  (RIFF/WAVE, or BW64/RF64 for files over 4 GiB). Add `profile: "dolby-atmos"` to normalize the model to
  the published Dolby Atmos Master ADM Profile constraints and fail closed on anything unfixable.
- `analysis.adm_inspect` — parse any ADM BWF back into a container/track/structure summary (round-trips
  your export and QCs third-party files).
- `analysis.adm_profile_check` — validate any ADM BWF against the profile and report a list of specific
  violations.

> Inseglet's ADM output is profile-*shaped* and self-validated against the published rules. Whether a
> specific certified renderer ingests a given file is something to confirm with your own certified
> toolchain (see `TRADEMARKS.md`).

### 8.4 Multi-step operations with one undo

`session.run_dsl` runs a small sequence of tool calls as a single atomic, single-undo operation, with
`$ref`/capture so later steps can use values produced by earlier ones. Use `dryRun: true` to see the
planned diff before applying.

---

## 9. Coordinate & channel conventions

Inseglet uses consistent conventions across the spatial and metering layers; the authority is
**`docs/CONVENTIONS.md`**. In brief: azimuth 0° is front and positive azimuth rotates counter-clockwise
(toward the left), matching the ambisonic/IEM convention used by the encoders and the metering read-back;
elevation is positive up. Bed channel order and layout labels (5.1, 7.1, 7.1.4, 9.1.6, 22.2) follow the
SMPTE/ITU-R BS.2051 conventions documented there. If you script placement by descriptors ("front-left",
"side-left", "voice-of-god", …), those resolve to positions in the same frame.

---

## 10. Troubleshooting

**Commands hang or nothing happens.** Make sure REAPER is in the **foreground** — App Nap on macOS
suspends the timer that drains the command queue. Bring REAPER to the front and retry.

**My rebuild didn't change anything.** Run `cmake --install build` after building — building alone does
not update the binary REAPER loads.

**The client can't connect.** Confirm REAPER is running with the extension loaded (you should see the URL
in the console). Check the discovery file (`reaper_mcp.json`) for the current **port and token** — the
port can change between runs. Ensure your client sends `Authorization: Bearer <token>` and targets
`http://127.0.0.1:<port>/mcp`.

**Auth fails (401).** The bearer token is per-run and stored in the discovery file; re-read it after
restarting REAPER.

**A destructive tool refuses to run.** It needs confirmation — either your client must support MCP
elicitation, or you must pass `confirm: true`.

**A spatial tool reports a plug-in isn't installed.** Some spatial tools drive third-party plug-ins (e.g.
the IEM suite, SPARTA) when present and fail closed with an install hint when they're not. Install the
named plug-in and retry.

**Integration tests.** The unit/protocol suite (`ctest -R unit`) runs without REAPER and is the supported
test tier for this preview. The end-to-end integration harness is currently a **skeleton** — see
`tests/README.md`; don't rely on it as coverage yet.

---

## 11. Security

Inseglet listens only on the **loopback interface** (`127.0.0.1`) and requires a **bearer token** for
every request. The token and port are written to the discovery file in your REAPER resource directory. On
a shared machine, treat that file as a secret (it grants control of your REAPER session) and restrict its
permissions accordingly. The full threat model — including why the transport is loopback-only and how
action-running is policy-gated — is in **`SECURITY.md`**. Report security issues privately per that file
rather than in a public issue.

---

## 12. Help, contributing, and license

- **Questions & ideas:** GitHub Discussions on the project repository.
- **Bugs & features:** GitHub Issues (templates provided).
- **Contributing:** see `CONTRIBUTING.md` — the unit/protocol suite runs without REAPER, so most changes
  can be developed and tested off-DAW.
- **License:** MIT (`LICENSE`); third-party components are credited in `THIRD_PARTY_NOTICES.md`.
- **Trademarks:** REAPER is a trademark of Cockos Incorporated; Dolby and Dolby Atmos are trademarks of
  Dolby Laboratories. Inseglet is independent and not affiliated with or endorsed by either — see
  `TRADEMARKS.md`.
