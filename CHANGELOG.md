# Changelog

All notable changes to Inseglet are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- Version reporting: the MCP `initialize` handshake now reports `serverInfo.version` as the package
  version; it had been hardcoded to an early `0.1.0` and was out of sync with the discovery file and the
  ReaPack package metadata (the index and package header are likewise brought to the current version).
- Windows build: the cross-platform release build failed to compile on MSVC with
  `error C2065: 'M_PI' undeclared` in the spatial tools. MSVC's `<cmath>` only defines `M_PI` when
  `_USE_MATH_DEFINES` is set before the header is included — glibc and Apple's libc++ expose it
  unconditionally, so the Linux and macOS builds were unaffected. Fixed by defining `_USE_MATH_DEFINES`
  project-wide for MSVC in `CMakeLists.txt`, and added an explicit `ws2_32` link on Windows so non-MSVC
  toolchains (MinGW / clang) link the loopback HTTP sockets too.
- Windows tests: the metering WAV round-trip test wrote scratch files to a hardcoded `/tmp/` path, which
  doesn't exist on Windows — the failed write left an empty buffer that was then read out of bounds and
  crashed the test. It now uses the OS temp directory via `std::filesystem::temp_directory_path()`.

## [1.5.0] — 2026-07-10

### Added
- **`analysis.object_decode_timeline`** — a read-only, purely geometric decode-coverage timeline for
  immersive objects, complementing `analysis.decode_coverage` (which decodes a whole ambisonic scene
  bus). For each object it reads the position trajectory — sampled from a live object track's panner
  automation (azimuth/elevation/distance) or supplied as explicit keyframes — and at every time slice
  encodes the object direction to SN3D spherical harmonics and projection-decodes it onto the chosen
  delivery layout's nominal loudspeakers, picking the dominant speaker. It reports the dominant-speaker
  timeline, the migration run-path, per-speaker time-weighted dwell fractions, the great-circle angular
  travel and elevation reach, and a time-weighted coverage footprint (hemisphere balance, uniformity,
  dead zones, speakers touched). Azimuth is +left / counter-clockwise, matching `spatial.ambisonic_encode`
  and `spatial.export_adm`; distance is carried for reporting only. It reads position metadata only —
  no render, no audio — auto-discovers the `Object N` tracks when no inputs are given, and fails closed
  on an unknown layout, empty keyframes, or more than 128 objects.

The direction encode, the projection-decode speaker basis, the dominant-speaker timeline, and the
dwell / travel / footprint statistics are a new SDK-free `src/decode_timeline.h`, unit-tested off-REAPER.
Surface is now **186 tools**.

## [1.4.0] — 2026-07-10

### Added
- **`spatial.orchestrate_sends`** — automatically wire and reconcile the bed-plus-object send layout
  that feeds an external Dolby Atmos Renderer (or DAPS) input bus. It routes the bed onto renderer
  channels 1…bed-width and each mono object onto one subsequent channel (bed first, matching the roster
  `spatial.export_adm` and `spatial.export_damf` emit), taking an explicit object-track list or
  auto-discovering the mono `Object N` tracks. Reconciliation is idempotent — an existing send already
  on the right channel is reused, a right-source/wrong-channel send is corrected in place, and only
  missing sends are added, so re-running never stacks duplicates. Stray sends with no roster slot are
  reported (and optionally pruned), and it fails closed on the Dolby master limits (at most 118 objects
  / 128 renderer channels). The renderer bus is resized and all sends reconciled inside a single undo
  block; a `dryRun` preview is the default.
- **`analysis.send_layout_inspect`** — read-only inspection and QC of a renderer bus's incoming
  receives: it decodes the send channel assignments, reconstructs the roster, and flags collisions,
  gaps, over-limit object/channel counts, bed-width mismatches, non-mono objects, and unrouted objects.

The send-channel roster and its encode/decode round-trip, the Dolby master constraint validator, the
idempotent reconciliation diff, and the read-back inspector are a new SDK-free `src/send_layout.h`,
unit-tested off-REAPER. Surface is now **185 tools**.

## [1.3.0] — 2026-07-10

### Added
- **`spatial.export_damf`** — author a native Dolby Atmos Master File (DAMF) triad: a `.atmos` YAML
  manifest (version, presentation, and the bed/object roster with 1-based input IDs), a
  `.atmos.metadata` YAML sidecar (sample rate plus a flat, ID-keyed event list — bed channels static,
  each object one event per trajectory block), and a `.atmos.audio` big-endian Core Audio Format (CAF)
  PCM file (interleaved essence, bed channels first then objects in roster order). It reuses the same
  model and stem-render path as `spatial.export_adm` (so `export_adm` is unchanged) and enforces the
  Dolby Atmos Master constraints — 48 kHz, a bed no larger than 7.1.2, and at most 118 objects / 128
  channels — failing closed on anything it cannot conform. The room frame is x left→right, y back→front,
  z floor→ceiling.
- **`analysis.damf_inspect`** — read-only inspection and QC of a DAMF triad: it round-trips the manifest
  roster, the metadata events, and the CAF header (with endianness warnings), and reports the same for a
  third-party master authored by another tool.

DAMF serialization is a new SDK-free `src/damf.h` (ADM-spherical → room coordinate map, big-endian CAF
writer, YAML manifest/metadata emitters, and the inspect parser), unit-tested off-REAPER. Surface is now
**183 tools**.

## [1.2.0] — 2026-07-10

### Added
- **Groove & quantization (MIDI).** `midi.quantize` (snap note starts to a grid with adjustable
  strength and swing; optionally snap note ends too), `midi.humanize` (seeded, reproducible timing and
  velocity randomization), `midi.apply_groove` (stamp a per-slot timing/velocity template — from
  `midi.extract_groove` or a named `swing8`/`swing16` — onto notes), `midi.extract_groove` (derive a
  reusable groove template from a reference take), `midi.transpose`, `midi.scale_velocity`,
  `midi.legato`, `midi.nudge`, and `midi.stretch`.
- **Takes & comping.** `take.add`, `take.delete`, `take.crop_to_active`, the per-take audio properties
  `take.set_vol` / `take.set_pan` / `take.set_pitch` / `take.set_playrate`, and `items.implode_to_takes`
  (stack items across tracks into one multi-take comp item).
- **Transport.** `transport.set_repeat` (loop on/off), `transport.set_playrate` (master play rate), and
  `transport.set_metronome` (click on/off).

The MIDI groove/quantization math is a new SDK-free `src/groove.h`, unit-tested off-REAPER. Surface is
now **181 tools**.

## [1.1.0] — 2026-07-10

### Added
- **`analysis.read_samples`** — direct, render-free PCM read of a track's item content or a take's
  source via a REAPER audio accessor (no render round-trip). Reports per-channel sample peak, RMS,
  oversampled true-peak, K-weighted level, DC offset, and full-scale clip count, plus an optional
  decimated min/max waveform overview. Bound the window with `start`/`duration`; resample with
  `sampleRate`.
- **`analysis.accessor_meter`** — the render-free companion to `analysis.meter`: per-channel level /
  peak / true-peak / K-level with SMPTE bed labels, L/R correlation, and BS.1770-4 gated loudness,
  computed in-box from accessor samples. A *track* target measures item content **before** track
  FX/volume/pan; `analysis.meter` remains the post-FX render measurement.
- **`analysis.detect_silence`** — frame-accurate leading/trailing silence, internal silent gaps, and
  the content-bearing span for trimming, from a windowed cross-channel RMS scan.

New `src/audio_accessor.h` handles the accessor lifecycle (create → validate → chunked read → destroy)
into the shared metering buffer; the SDK-free helpers (DC offset, clip count, waveform overview,
silence scan) live in `src/ambisonic_meter.h`. Surface is now **161 tools**.

### Fixed
- The extension now reports its release version (**1.1.0**) in the discovery file and build metadata;
  earlier builds reported `0.1.0` regardless of the published release.

## [1.0.1] — 2026-07-10

### Fixed
- **Surround-panner azimuth handedness.** `spatial.set_source_position` and
  `spatial.spatialize_stems` placed a positive azimuth on the *right* of a channel bed —
  the opposite of the ambisonic encoder, ADM writer, and metering, where `+az = left`.
  ReaSurroundPan's `in 1 X` parameter runs `0 = right … 1 = left`, so the panner tools now
  negate the x coordinate when writing to it; a channel bed and an ambisonic scene now agree
  on handedness. (`y`/`z` unchanged.)
- **`analysis.check_deliverable` bus measurement.** It now measures the requested bus/track
  instead of silently grading the master mix when the target is not the sole route to the
  master (`RENDER_SETTINGS` source fix).

## [1.0.0] — 2026-07-10

Initial public release. Inseglet is a native REAPER extension that exposes a
Model Context Protocol (MCP) interface to the DAW, with immersive/spatial audio and
native ADM (Dolby Atmos) authoring as its focus. 158 tools; live-verified on macOS.

### Core control surface
- Project, track, item, take, envelope, FX, send/routing, tempo, and master control
  over a JSON-RPC 2.0 MCP server (protocol 2025-06-18), with bearer-token auth bound
  to a loopback port.
- Granular state setters with consolidated reads; elicitation-gated wholesale state
  replacement with dry-run diffs; an action/command passthrough; and resource
  subscriptions for event-driven workflows.

### Immersive & spatial audio
- Channel beds from 5.1 through 7.1, 7.1.4, 9.1.6, and 22.2, with correct LFE handling
  and layout-aware surround positions.
- Ambisonic scene work — encode/decode, order and format conversion, scene rotation
  (yaw/pitch/roll, head-tracking ready), mirroring, beamforming, and distance/room
  placement — via IEM and SPARTA plug-ins.
- Binaural monitoring with live head-tracking.

### Native ADM / Dolby Atmos authoring
- Author ITU-R BS.2076 ADM Broadcast-Wave deliverables in-box (bed plus object
  trajectories), with per-object metadata (extent, divergence, importance) and
  round-trip inspection.
- Dolby Atmos Master ADM Profile conformance mode, plus a standalone conformance
  validator for any ADM BWF.

### Metering & analysis
- Immersive metering beyond LUFS: loudness, per-channel and correlation metering;
  ambisonic spatial-field analysis (direction of arrival, diffuseness, energy vectors);
  object, stem, dialog, and downmix loudness with timelines; binaural checks; and
  decode-coverage analysis.

### Agent ergonomics
- MCP prompts and a deterministic macro-DSL for composing multi-step operations, with
  outcome-level spatial and deliverable-conformance verbs.

Runs on REAPER for macOS; Windows and Linux load-verification are in progress. See
[`docs/MANUAL.md`](docs/MANUAL.md) to get started and [`docs/REFERENCE.md`](docs/REFERENCE.md)
for the full tool, resource, and prompt reference.
