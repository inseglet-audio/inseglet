# Inseglet

Inseglet is a native REAPER extension that exposes a first‑class **Model Context Protocol (MCP)**
interface to Cockos REAPER, built around **immersive / spatial audio** and native **ADM (Dolby Atmos)**
authoring. It runs in‑process for real‑time, event‑driven control of the DAW, and gives an MCP client a
large, well‑documented tool surface for editing, mixing, spatial scene work, immersive metering, and
object/bed deliverables.

The loaded extension binary is named `reaper_mcp.*`.

> **New here?** Start with:
>
> - [`docs/REFERENCE.md`](docs/REFERENCE.md) — the full tool / resource / prompt reference, generated
>   from the live registry.
> - [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) — channel order, coordinate math, bed layouts, ambisonic
>   wiring, and the named loudness/deliverable specs.
> - [`docs/MANUAL.md`](docs/MANUAL.md) — the user manual (install, connect a client, tool catalog, workflows).
> - [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to build, test, and open a pull request. **Contributions welcome.**
> - [`SECURITY.md`](SECURITY.md) — the loopback‑only transport threat model.

## What you can do with it

- **Core editing** — transport, project, tracks, markers & regions, items, track FX, routing / sends,
  automation envelopes, timeline & tempo, takes + MIDI note/CC editing, track state‑chunk read/patch/write,
  freeze/metering, and an always‑on `tools.enumerate` meta‑tool.
- **Spatial / immersive** — channel beds (5.1 → 22.2), ambisonic encode/decode (ACN/SN3D, up to 7th order),
  order/format conversion, scene rotation/mirroring, beamforming, distance/room, surround panners, one‑call
  Atmos session scaffolding, binaural monitoring, and live OSC head‑tracking.
- **Immersive analysis / metering** — a BS.1770‑4 gated‑loudness engine (program, stem, dialog, per‑object),
  true‑peak (dBTP), ambisonic spatial‑field analysis (DirAC direction / diffuseness / Gerzon `rV`/`rE`),
  downmix and binaural QC, decode‑coverage, and loudness timelines — in an SDK‑free DSP core.
- **Object / Atmos deliverables** — `spatial.export_adm` authors an ITU‑R **BS.2076 ADM** Broadcast‑Wave
  (bed + object trajectories, per‑object extent/divergence/importance), with a conformance mode shaped to
  the published **Dolby Atmos Master ADM Profile** and `analysis.adm_profile_check` to validate any ADM BWF.
- **Semantic / agentic** — named loudness/true‑peak deliverable specs, immersive‑aware style chains with a
  bundled multichannel true‑peak limiter, a deterministic composite macro‑DSL (`session.run_dsl`), and
  expert prompts.

## Design

Inseglet compiles against the REAPER C SDK and hosts the MCP server **in‑process**, which is what enables
its real‑time, event‑driven behavior:

- **Direct API access** via `reaper_plugin_info_t::GetFunc` — the full native surface, no intermediate bridge.
- **Main‑thread safety and a real pump** via `IReaperControlSurface::Run()` (called on the main thread every
  UI tick), draining a thread‑safe command queue.
- **Push‑based state** via the surface `Set*` callbacks → MCP resource subscriptions, so a client can react
  to changes instead of polling.
- **Sub‑millisecond enqueue**, with bounded per‑tick execution to keep the UI fluid.

A **zero‑install fallback** — a `defer` ReaScript driven through REAPER's built‑in web server via an EXTSTATE
relay — lets you try a reduced toolset before installing the binary, and doubles as an independent test
oracle (see `bootstrap/`).

Destructive verbs (for example `spatial.setup_immersive_session` on a non‑empty project, or
`track.set_state_chunk`) confirm via a real async **MCP elicitation** round‑trip: the server validates on the
main thread, answers on an SSE stream carrying `elicitation/create`, awaits the human **off** the main thread
(so REAPER keeps pumping), then applies the change on accept. Clients without elicitation use an explicit
`confirm:true`.

```
MCP client ──MCP (Streamable HTTP / stdio)──►  reaper_mcp extension
                                               │  worker thread: MCP server
                                               │        │ enqueue command
                                               │        ▼
                                               │  MainThreadQueue (thread‑safe)
                                               │        │ drained on the main thread
                                               │        ▼
                                               │  ControlSurface::Run()  ──►  REAPER C API
                                               │  Set* callbacks  ──►  resource notifications
```

## Building

```bash
git clone https://github.com/inseglet-audio/inseglet.git && cd inseglet
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release -R unit          # host-core unit + protocol suite, no REAPER
cmake --install build                              # deploys the extension (and the bundled JSFX)
```

Prereqs: CMake ≥ 3.20 and a C++17 compiler (MSVC on Windows for SDK ABI compatibility, AppleClang on macOS
for a universal `x86_64;arm64` build, GCC/Clang on Linux). The build fetches the REAPER SDK, WDL,
nlohmann/json, and cpp‑httplib headers and produces the platform extension binary:

| Platform | Artifact | Install path |
|---|---|---|
| macOS | `reaper_mcp.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| Windows | `reaper_mcp.dll` | `%APPDATA%\REAPER\UserPlugins\` |
| Linux | `reaper_mcp.so` | `~/.config/REAPER/UserPlugins/` |

> **`cmake --install` is required after every rebuild** — it copies the binary into the `UserPlugins`
> directory REAPER loads from and ships the bundled JSFX to `Effects/MCP/`.

Restart REAPER (keep it in the **foreground** — macOS App Nap suspends the UI timer and stalls the pump). The
server prints its loopback URL + bearer token to the REAPER console and writes a discovery file at
`~/Library/Application Support/REAPER/reaper_mcp.json`. Point your MCP client at `http://127.0.0.1:<port>/mcp`
with that token. See [`docs/MANUAL.md`](docs/MANUAL.md) for the full walkthrough.

## Status

Phases 1–8 are complete and live‑verified in REAPER on macOS. The in‑process MCP server (Streamable
HTTP + SSE + JSON‑RPC 2.0, MCP `2025‑06‑18`), the main‑thread command pump, and a live
`IReaperControlSurface` host **186 tools**, plus MCP resources and expert prompts. The extension compiles
against the pinned REAPER SDK **and** an SDK‑free host core; the unit + protocol suite (`ctest -R unit`)
drives the full request lifecycle and the elicitation round‑trip over real HTTP with no REAPER required.
CI builds the macOS / Windows / Linux matrix on every push. **v1 targets macOS first**; Windows and Linux
follow once load‑verified in a real REAPER (both already compile in CI). The integration test tier is
currently a skeleton — see [`tests/README.md`](tests/README.md).

## Maintainer

Created and maintained by **James Livingston** ([@jlivingston-Cipher](https://github.com/jlivingston-Cipher)).
Issues and pull requests are welcome — start with [`CONTRIBUTING.md`](CONTRIBUTING.md). See
[`MAINTAINERS.md`](MAINTAINERS.md) for governance.

## License

Released under the **MIT License** — see [`LICENSE`](LICENSE). © 2026 James Livingston. Third‑party
components are credited in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Trademarks

REAPER is a trademark of Cockos Incorporated. Dolby and Dolby Atmos are trademarks of Dolby Laboratories.
All other product and company names are the trademarks of their respective owners. Inseglet is an
independent, community‑developed extension and is **not affiliated with, authorized, sponsored, or endorsed
by** Cockos, Dolby, or any other trademark holder. See [`TRADEMARKS.md`](TRADEMARKS.md) for details,
including the scope of the ADM / Dolby Atmos Master ADM Profile support.
