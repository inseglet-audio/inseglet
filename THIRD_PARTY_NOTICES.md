# Third-Party Notices

Inseglet is distributed under the MIT License (see `LICENSE`). It builds against, bundles, or adapts the
third-party components below, each under its own license. This file collects the notices those licenses
require. Dependency versions are pinned in `cmake/FetchDeps.cmake` and `cmake/FetchReaperSDK.cmake`.

---

## Bundled / fetched at build time

### nlohmann/json — MIT License
JSON value type and (de)serialization for the JSON-RPC / MCP payloads. Fetched at configure time
(`cmake/FetchDeps.cmake`, pinned tag).
Copyright © 2013–2025 Niels Lohmann. <https://github.com/nlohmann/json>

### cpp-httplib — MIT License
Header-only HTTP/1.1 server (Streamable HTTP + SSE) for the loopback transport. Fetched at configure time
(`cmake/FetchDeps.cmake`, pinned tag).
Copyright © 2017 Yuji Hirose. <https://github.com/yhirose/cpp-httplib>

### REAPER C/C++ Extension SDK and WDL — Cockos Incorporated
Headers used to compile the extension against REAPER's native API. Used under Cockos's permissive SDK/WDL
terms; not statically linked into any redistributable beyond the headers required to build the extension.
<https://www.reaper.fm/sdk/> · <https://www.cockos.com/wdl/>

*(The MIT license text for nlohmann/json and cpp-httplib is reproduced in full in the `LICENSES/`
directory of a source release; upstream `LICENSE` files travel with the fetched sources.)*

---

## Adapted source

### CPython `difflib` — Python Software Foundation License, Version 2
`src/track_chunk.h` includes a C++ port of the unified-diff logic from CPython's `difflib`
(`_format_range_unified`, `get_grouped_opcodes`, `get_opcodes`), used to produce human-readable diffs for
track state-chunk dry-runs. The PSF License is permissive and MIT-compatible; this notice preserves the
required attribution.
Copyright © 2001–2025 Python Software Foundation. All rights reserved. <https://docs.python.org/3/license.html>

---

## Referenced standards and DSP methods

The following are cited where implemented (they are specifications and published methods, not bundled code):

- **ITU-R BS.1770** (loudness / true-peak), **BS.2051** (advanced sound-system layouts), **BS.2076** (Audio
  Definition Model), **BS.2088** (BW64) — International Telecommunication Union.
- **EBU R 128**, **EBU Tech 3341 / 3342** (loudness metering) — European Broadcasting Union.
- **Dolby Atmos Master ADM Profile** — published constraints used by the ADM conformance mode. See `TRADEMARKS.md`.
- BS.1770 K-weighting filter coefficients follow the widely used reference implementations; DirAC
  direction/diffuseness follows Pulkki, *Directional Audio Coding* (2007/2009); the Gerzon energy/velocity
  vectors follow Gerzon, *General Metatheory of Auditory Localisation* (AES, 1992).

## Interoperability (not bundled)

Inseglet can drive third-party plug-ins when they are installed on the user's system (for example the
**IEM Plug-in Suite** and **SPARTA/COMPASS**), and can hand off to external renderers. These tools are not
bundled, modified, or redistributed with Inseglet; they remain under their own licenses and are the property
of their respective authors.
