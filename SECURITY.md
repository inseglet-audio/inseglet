# Security

`reaper_mcp` is a native REAPER extension that hosts an in-process MCP server so an MCP client (an
LLM agent) can drive a **local** REAPER instance. This document states the threat model, the controls,
the known limitations, and how to report a vulnerability. It is the Phase 7 security-review deliverable.

## Threat model — what this is

The server is a **loopback-only, single-user, local** service. It is *not* a remote API and is not
intended to be exposed to a network. The trust boundary is the local machine: a process that can reach
`127.0.0.1` **and** present the per-launch token can drive REAPER through the full tool surface (create/
delete tracks, insert FX, render, etc.). The token is the gate; everything behind it is as privileged as
the user running REAPER.

Out of scope: multi-tenant/remote deployment, untrusted network exposure, and sandboxing REAPER itself.

## Controls

- **Loopback bind only.** The HTTP server binds `127.0.0.1` on an **OS-assigned ephemeral port** (never a
  fixed, guessable port; never `0.0.0.0`). Nothing is reachable off-host without the user deliberately
  forwarding the port.
- **Bearer-token auth on every request.** A **CSPRNG** token is minted at launch and required on every
  JSON-RPC request; unauthenticated requests are rejected. The token rotates each launch.
- **Out-of-band token delivery.** The url + token are written to a per-launch **discovery file**
  (`reaper_mcp.json`) in the REAPER resource directory — read by a local client the user controls, never
  transmitted over the wire by the server.
- **No dynamic code execution.** Tools are fixed, compiled C++ handlers. The composite macro-DSL
  (`session.run_dsl`) is a **deterministic** macro over the fixed verb set — it does **not** evaluate
  arbitrary code and there is **no LLM in the execution path**; a script resolves to the same tool calls
  every time. There is no `eval`, no shelling out, no plugin/script loading driven by request content.
- **Destructive-action confirmation.** Destructive verbs (today `spatial.setup_immersive_session` on a
  non-empty project) require confirmation via a real MCP **`elicitation/create`** round-trip, falling back
  to an explicit `confirm:true` for clients that don't advertise elicitation. Nothing mutates before the
  human answer; decline/cancel/timeout mutate nothing.
- **Bounded main-thread work + single-undo mutations.** Tool work is marshaled to REAPER's main thread in
  bounded slices (UI stays responsive), and each mutation is a single undo point — a mistaken call is one
  Ctrl/Cmd-Z away.
- **No telemetry / no egress.** The extension does not phone home; it operates only on the local REAPER
  session, the project, and render outputs the user directs it to.

## Known limitations (honest)

- **The token is the whole gate.** Any local process that can read the discovery file (or otherwise obtain
  the token) and reach the loopback port has full tool access. Protect the discovery file; treat the token
  as a local secret.
- **Loopback traffic is not encrypted.** There is no TLS — acceptable for `127.0.0.1`, but do not tunnel the
  port to another host without adding transport security in front of it.
- **Discovery-file permissions follow the process umask.** The token file should be user-readable only;
  verify your umask if the machine has other local users. (Hardening candidate: force restrictive perms on
  write.)
- **Elicitation concurrency.** The elicitation lane holds one HTTP worker thread per pending human prompt.
  This is fine for the single-session model; if a future release exposes concurrent clients, the pool sizing
  and per-prompt threading must be revisited.

## Reporting a vulnerability

Report suspected vulnerabilities **privately** to the repository owner (do not open a public issue for a
security problem). Include the REAPER version, the extension version, and a minimal reproduction. You will
get an acknowledgement and a fix timeline; please allow a reasonable window before any public disclosure.
