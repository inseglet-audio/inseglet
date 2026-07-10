# bootstrap/ — zero-install fallback transport

`reaper_mcp_bridge.lua` is a **defer ReaScript** that lets the MCP server talk to REAPER **without
the native extension installed**, using REAPER's built-in Web Remote server + an ExtState relay.

It exists for two reasons:

1. **On-ramp.** Users can try a reduced MCP toolset immediately (no compiled binary, no code
   signing) — lowering adoption friction while the native build lands on their platform.
2. **Test oracle.** Because it reaches REAPER through a completely different mechanism than the
   native extension, it serves as an independent implementation to cross-check native results in CI.

## Trade-offs vs the native path

| | Native extension | This fallback |
|---|---|---|
| Latency | sub-ms enqueue, main-thread pump | ~30 Hz defer poll |
| Push events | yes (control-surface callbacks) | no (polling only) |
| API reach | full native + C-only | ReaScript subset |
| Install | compiled/signed binary | drop a `.lua`, enable web interface |

The MCP client prefers the native extension whenever it is detected and falls back to this bridge
otherwise.

## Setup
See the header comment in `reaper_mcp_bridge.lua`. In short: load it as a ReaScript action, run it
(it self-re-arms), and enable a *Web browser interface* under Preferences ▸ Control/OSC/web.
