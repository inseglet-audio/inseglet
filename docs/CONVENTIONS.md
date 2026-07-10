# REAPER MCP — Immersive & Spatial Conventions

This document is the single source of truth for the channel-order, coordinate, bed-layout, ambisonic,
and loudness conventions the spatial tool surface encodes. Values here are lifted directly from the
implementation (`src/tools/tools_spatial.cpp`, `src/spatial_verbs.h`, `src/deliverable_specs.h`) — if the
code and this document ever disagree, the code wins and this document is stale. See `docs/REFERENCE.md`
for the per-tool contracts and `SECURITY.md` for the transport threat model.

Standards basis throughout: loudness and true-peak per **ITU-R BS.1770-5** (K-weighted LKFS/LUFS +
inter-sample true peak), metered per **EBU Tech 3341** ("EBU Mode"); immersive speaker layouts per
**ITU-R BS.2051-3 / BS.775**; the 22.2 grouping per **SMPTE ST 2036-2**; channel beds in SMPTE / Dolby
Atmos **film order**.

## 1. Channel-based beds

A bed bus is a single multichannel track whose channel *positions* map to fixed speakers. Ordering is
**film order**: `L R C LFE`, then surrounds, then heights. Labels below are position → speaker; the LFE
column gives the **0-based** channel index(es).

| Layout | Channels | Channel order (position → speaker) | LFE index (0-based) | Notes |
| --- | --- | --- | --- | --- |
| `5.1` | 6 | L, R, C, LFE, Ls, Rs | 3 | SMPTE/ITU 5.1 film order. |
| `7.1` | 8 | L, R, C, LFE, Lss, Rss, Lrs, Rrs | 3 | Side (Lss/Rss) then rear (Lrs/Rrs) surrounds. |
| `7.1.4` | 12 | L, R, C, LFE, Lss, Rss, Lrs, Rrs, Ltf, Rtf, Ltr, Rtr | 3 | Dolby Atmos 7.1.4 bed: 7.1 + 4 tops (front Ltf/Rtf, rear Ltr/Rtr). |
| `9.1.6` | 16 | L, R, C, LFE, Lss, Rss, Lrs, Rrs, Lw, Rw, Ltf, Rtf, Ltm, Rtm, Ltr, Rtr | 3 | Atmos 9.1.6 bed: 7.1 + wides (Lw/Rw) + 6 tops (front/middle/rear). |
| `22.2` | 24 | FL, FR, FC, LFE1, BL, BR, FLc, FRc, BC, LFE2, SiL, SiR, TpFL, TpFR, TpFC, TpC, TpBL, TpBR, TpSiL, TpSiR, TpBC, BtFC, BtFL, BtFR | 3, 9 | NHK 22.2 (SMPTE ST 2036-2: 10-ch middle w/ 2 LFE, 9-ch upper, 3-ch lower). |

`spatial.build_bed` inserts a track of the correct width, names it, and returns the speaker channel map.
`analysis.check_deliverable` auto-detects bed buses by these widths (6/8/12/16/24) for per-bed
sub-metering; anything else is treated as a non-bed track.

> **22.2 caveat.** The 22.2 interleave order is renderer-dependent. The map above is the SMPTE ST 2036-2
> grouping; always confirm the exact channel interleave against your target renderer before committing a
> deliverable.

## 2. LFE handling

The LFE occupies **channel index 3 (0-based)** in every 5.1 / 7.1 / 7.1.4 / 9.1.6 bed; 22.2 carries two,
at indices **3 (LFE1)** and **9 (LFE2)**. Consequences the tools enforce:

- An LFE-tagged source is routed as a **mono send to the bed's LFE channel** (band-limited), never
  panned through ReaSurroundPan. In an ambisonic scene there is no LFE channel — an LFE source is sent
  full-band to the scene bus, and the tools flag that the LFE band is not meaningful in a soundfield.
- The bundled multichannel true-peak limiter (`mix.apply_style` on a wide immersive bed) **excludes the
  LFE from the linked gain computation** and passes it time-aligned but unlimited — limiting the LFE
  along with the mains would pump the whole bed off sub-bass transients. The LFE channel set is given to
  the JSFX on a 1-based slider, not pin-wired.

## 3. Ambisonics

- **Channel order: ACN.** Ambisonic Channel Number ordering is the modern default (IEM, SPARTA, ambiX).
  The encoder's ACN output pin *k* is identity-wired to track channel *k* via `TrackFX_SetPinMappings`.
- **Normalization: SN3D (default).** `N3D` and legacy `FuMa` are also accepted (`normalization` param).
  SN3D is the ambiX convention and the default across IEM/SPARTA.
- **Channel count = (order + 1)².** The track is sized to hold the full ACN set; any surplus channels
  are left silent. Both `hoaChannels` (the real ACN count) and `trackChannels` are reported.

| Order | ACN channels (order+1)² |
| --- | --- |
| 1 (first) | 4 |
| 2 (second) | 9 |
| 3 (third) | 16 |
| 4 (fourth) | 25 |
| 5 (fifth) | 36 |
| 6 (sixth) | 49 |
| 7 (seventh) | 64 |

- **Suite priority: IEM > SPARTA > ATK** (also recognizes generic `ambiX`). `spatial.detect_spatial_suites`
  discovers what's installed; the setup verbs prefer IEM.
- **IEM angle params are NORMALIZED `[0,1]`, not degrees.** REAPER's FX param API exposes IEM/JUCE angle
  parameters as a normalized `[0,1]` value (0.5 = 0°). The tools discover each param's real degree span
  from the plug-in (`TrackFX_FormatParamValueNormalized` at the endpoints) and map requested degrees onto
  that span, rather than writing raw degrees that would peg to an extreme. JSFX real-degree params are
  written through directly, clamped to range. (Observed: IEM StereoEncoder reports ±180° for **both**
  azimuth and elevation.)

## 3a. Ambisonic transforms (Batch S1)

Batch S1 adds order/format/mirror/beam/room depth. The exact math for the deterministic ones is here; it is
implemented in the two bundled zero-dependency JSFX (`src/jsfx/mcp_ambi_convert.jsfx`,
`src/jsfx/mcp_ambi_mirror.jsfx`, shipped to `Effects/MCP/`). Channel model throughout: ACN index `k` has
degree `l = floor(√k)` and order `m = k − l² − l` (so `m ∈ [−l, +l]`); `|m|` is the associated-Legendre
degree used by the real spherical harmonics.

**Order change (`spatial.convert_order`).** Pure track-width resize, no FX. Target width = `clampChannels((toOrder+1)²)`
(even-padded, §3). **Down**-order drops the ACN channels beyond `(toOrder+1)²` — an exact operation, since the
retained lower-degree channels are unchanged and form a valid lower-order scene. **Up**-order widens the bus and
leaves the new higher-degree channels silent (zero-pad) — it synthesizes no spatial detail and says so in a
warning. `fromOrder` is inferred as the largest `o` with `(o+1)² ≤ current channel count` when omitted.

**Normalization / format (`spatial.convert_format`, JSFX modes).**
- **SN3D → N3D:** multiply ACN channel `k` by `√(2l+1)`. **N3D → SN3D:** divide by `√(2l+1)`. Exact, any order
  (identity channel map, per-degree scalar).
- **ambiX (ACN/SN3D) → FuMa (first order only):** reorder `W Y Z X → W X Y Z` and scale `W · 1/√2`.
  **FuMa → ambiX:** reorder `W X Y Z → W Y Z X` and scale `W · √2`. Above first order FuMa max-N has no single
  agreed convention → refused; keep HOA in SN3D/N3D. `ambiX ≡ ACN/SN3D`. `N3D↔FuMa` is done via SN3D.

**Reflection (`spatial.mirror_scene`, JSFX sign tables).** A scene reflection is a pure per-ACN-channel sign
flip on the real SH — exact and order-independent. For channel `(l, m)`, `am = |m|`:

| Plane | Physical effect | Per-channel sign |
| --- | --- | --- |
| `left-right` | swap L ↔ R (reflect across the front/back vertical plane; azimuth φ→−φ) | `−1` if `m < 0`, else `+1` (flip the sine harmonics) |
| `front-back` | swap front ↔ back (φ→π−φ) | `m ≥ 0`: `(−1)^am`; `m < 0`: `(−1)^(am+1)` |
| `up-down` | swap up ↔ down (elevation θ→π−θ) | `(−1)^(l+am)` (elevation parity, azimuth-agnostic) |

Sign tables are independent of normalization (SN3D vs N3D), so mirroring is correct for either. A reflection is
NOT a rotation (it flips handedness) — this complements, and does not duplicate, `spatial.rotate_scene`.

**Beam / room (`spatial.beamform`, `spatial.set_distance`).** Plug-in-orchestrated (SPARTA Beamformer / IEM
RoomEncoder), same resolve-and-report substrate as encode/rotate/decode; no in-box matrix. `set_distance` places
the source at `distanceM` along (az, el) in **RoomEncoder's own room frame — `+X = front, +Y = left, +Z = up`
(the ambiX convention of §3b, `+az = left`), NOT the §4 panner frame** — so `X = d·cos(el)·cos(az)`,
`Y = d·cos(el)·sin(az)`, `Z = d·sin(el)`, each mapped onto the plug-in's own metric range (`setNamedRealParam`
parses the formatted endpoints — no degree guard, so metres/counts map correctly). The tool also **pins the
listener to the room centre** (`listener position x/y/z` → 0 m) so the `(az, el)` placement is measured from the
listener rather than the room origin (RoomEncoder's default listener is offset). Both fail closed with an install
hint when the suite is absent.

## 3b. Ambisonic metering (Batch M1)

`analysis.spatial_field` reads the samples of a bounded, **bit-exact** render of an ACN/SN3D scene bus (no
normalize/limit, so the inter-channel amplitude cues survive) and reports the field. All math is in the
SDK-free `src/ambisonic_meter.h` and unit-tested host-side (`unit.meter`). Channel model as §3a: ACN index
`k` has degree `l = floor(√k)`, order `m = k − l² − l`. Assumed SN3D (pass `normalization:"n3d"` to divide the
first-order dipoles by `√3` first). The direction frame is the §4 project frame **x = right, y = front,
z = up**; ambiX SH axes are **X = front, Y = left, Z = up** with azimuth CCW-from-front, so the analyzer maps
`(front, left, up) = (y, −x, z)` before evaluating the harmonics. **Reported azimuth is the ambisonic-standard
convention — CCW from front, `+az = LEFT`** (ACN/SN3D/ambiX, as IEM/SPARTA and `spatial.ambisonic_encode` use),
so a source the encoder places at azimuth θ reads back at DoA azimuth θ. The surround-panner tools (§4) now use this same `+az = left` convention, so azimuth is consistent across the panner, encoder, ADM writer, and metering. Elevation `+90° = overhead`.

- **First-order intensity mapping (SN3D).** For a plane wave, `W = s` and the SN3D dipoles equal the
  direction cosines, so `[ACN3, ACN1, ACN2] = s·[front, left, up]`. The active-intensity vector in the
  project frame is `I = (−⟨W·ACN1⟩, ⟨W·ACN3⟩, ⟨W·ACN2⟩)` (right, front, up).
- **Direction of arrival.** `az = atan2(−I_x, I_y)`, `el = atan2(I_z, hypot(I_x, I_y))` — ambisonic-standard
  azimuth (0° = front, **+90° = left**, CCW), elevation +90° = overhead. (`I_x` is the right-component, so
  `−I_x` is the left-component the ambisonic azimuth is measured toward — the round-trip with the encoder.)
- **Diffuseness (DirAC).** `ψ = 1 − ‖⟨I⟩‖ / ⟨E⟩` with energy density `E = ½(W² + (ACN1²+ACN2²+ACN3²))`.
  `ψ = 0` for a single plane wave, `ψ → 1` for a decorrelated/diffuse field. `directivity = 1 − ψ`.
- **Velocity vector rV** `= ⟨W·u⟩ / ⟨W²⟩` (u = the first-order dipoles in the project frame). `|rV| = 1` for
  a plane wave, `→ 0` diffuse; direction agrees with the intensity DoA.
- **Energy vector rE.** A projection decode onto a near-uniform **Fibonacci-sphere** virtual-speaker layout
  (default 36 speakers): per-speaker gain `g_s = Σ_k a_k · Y_k^{SN3D}(d_s)`, energy `E_s = ⟨g_s²⟩`, then
  `rE = Σ_s E_s d_s / Σ_s E_s`. Uses the real SN3D spherical harmonics up to the detected order, so `|rE|`
  sharpens with order (a decoder-quality localization readout). `|rE| < 1`.
- **Real SN3D SH.** `Y_l^m = N_l^{|m|} · P_l^{|m|}(sin el) · {cos, 1, sin}(|m|·az)` for `m {>,=,<} 0`, with
  `N_l^{|m|} = √((2 − δ_{m0})·(l−|m|)!/(l+|m|)!)` and the associated Legendre `P` taken **without** the
  Condon-Shortley phase (ambiX convention).

`analysis.meter`'s per-channel loudness uses the ITU-R BS.1770 **K-weighting** (a high-shelf + an RLB
high-pass biquad, designed at the file's sample rate by bilinear transform) and **true peak (dBTP)** via 4×
oversampling; program-level loudness (integrated LUFS / LRA / true peak) still comes from REAPER's native
`RENDER_STATS` (§5). LFE channels (index 3, and index 9 for 22.2 — §2) are flagged and excluded from the
BS.1770 program measure.

## 3c. Gated loudness, timelines, dialog, downmix (Batch M2)

Batch M2 adds a **C++ BS.1770-4 gated-loudness engine** to `ambisonic_meter.h` for signals REAPER's
`RENDER_STATS` cannot measure directly — stems read back from a bit-exact render, an isolated dialog bus,
and downmixes that exist only in memory. Program loudness of a rendered master stays REAPER-native (the M1
convention); the C++ engine carries the per-stem / dialog / timeline / fold metrics. All of it is SDK-free
and proven in `unit.meter`.

- **Gated integrated loudness (BS.1770-4).** 400 ms blocks at 75 % overlap (100 ms hop); block loudness
  `l_j = −0.691 + 10·log₁₀(Σ_ch G_ch·z̄²_ch,j)`; **absolute gate** −70 LKFS, then **relative gate** 10 LU
  below the mean power of the surviving blocks. A range shorter than one block falls back to the ungated
  measure. `activityFraction` = surviving blocks / total blocks.
- **Channel weights `G`** come from the §2 SMPTE bed labels: fronts + heights + wides **1.0**, surrounds
  (`Ls/Rs/Lss/Rss/Lsr/Rsr`) **1.41**, **LFE excluded** (weight 0; index 3, and 9 for 22.2). Unlabeled
  widths weigh every channel 1.0.
- **Dialog loudness (`analysis.dialog_loudness`).** Dialog level = the gated loudness of the **isolated
  dialog stem** (bit-exact stem render): the gates drop inter-phrase silence, so the number approximates
  speech-active level. This is an honest approximation — valid when the stem carries dialog only — and is
  **not** a VAD (not Dolby Dialogue Intelligence). Program loudness comes from `RENDER_STATS` (C++ gated
  fallback if the stats carry no loudness tokens). `programToDialogLu = program − dialog` (dialnorm-style;
  reference point: Netflix dialogue-gated −27 LKFS ±2 LU).
- **Momentary / short-term timeline (`analysis.loudness_timeline`).** Momentary = 400 ms window,
  short-term = 3 s window, both on a common hop (default 100 ms; auto-coarsened to cap the series at
  `maxPoints`). Times are the window **end**, in seconds from the start of the analyzed range; the
  short-term window **grows** until 3 s of context exists (meter-startup behavior). **LRA** follows EBU
  Tech 3342 on the full-3 s short-term population: −70 absolute gate, −20 LU relative gate, LRA = 95th −
  10th percentile.
- **Stereo fold-down (`analysis.downmix_check`).** ITU-R BS.775 practice from the bed labels: `L/R` at
  0 dB, `C` at −3 dB (1/√2) to both, every other labeled channel to its side at −3 dB (BS.775 predates
  heights/wides — the −3 dB side-fold is the documented practical convention), **LFE omitted** by default
  (`includeLfe:true` folds it at −3 dB to both). Mono fold = `(Lo+Ro)/2`. Interpretation bands on
  `monoVsStereoLu`: ≥ −3.5 mono-compatible (correlated content folds at ≈ −3 LU); −3.5…−6.5 decorrelated
  (expected loss); < −6.5 **phase cancellation**. Supported widths: 2 / 5.1 / 7.1 / 7.1.4 / 9.1.6.
- **Windowed spatial field (`analysis.spatial_field timeline:true`).** The §3b analysis evaluated per
  window (default 1 s window / 0.5 s hop, hop auto-coarsened to ≤ 400 windows); each window reports DoA
  az/el, diffuseness, |rV|, |rE| (+ rE direction) — the trajectory of a moving source. Window times are
  the window **start**.

## 3d. Object loudness, binaural QC, decode coverage (Batch M3)

Batch M3 rounds out the metering suite with the object model, the headphone deliverable, and spatial
coverage. `object_loudness` and `binaural_check` reuse the §3c gated engine + §3b per-channel/correlation
DSP; `decode_coverage` adds a virtual-speaker decode to `ambisonic_meter.h`. All SDK-free and proven in
`unit.meter`.

- **Object loudness (`analysis.object_loudness`).** Each object track is rendered as its own bit-exact
  stem and measured as a single stream with the §3c BS.1770-4 gated engine (mono objects weigh 1.0). An
  object's **energy share** is its ungated linear power divided by the total object power
  (`share_i = p_i / Σ p`) — linear power **is** additive across stems, unlike LUFS, so the shares sum to
  1. `deltaFromLoudestLu` is the gated offset from the loudest object. The **position** is read
  best-effort from the object's first panner/encoder FX exposing azimuth / elevation / distance params
  (formatted via `TrackFX_FormatParamValueNormalized` — REAPER exposes IEM/JUCE angles as normalized
  `[0,1]`, §3), and is `null` when no positional panner is present. An optional `bed:` renders the bed
  bus for a **level reference** (`bedVsObjects = bedLUFS − loudestObjectLUFS`) — a comparison, not a sum.
- **Binaural QC (`analysis.binaural_check`).** A 2-channel binaural monitor bus (§ Phase-4
  `spatial.add_binaural_monitor`) or stereo binaural render is measured measure-don't-limit (`RENDER_STATS`
  program loudness/true-peak on a headroomed bus ⇒ bit-exact samples). Beyond program loudness it reports
  **L/R correlation** (mono compatibility / image integrity), the **inter-aural level balance**
  `balance = L_Klevel − R_Klevel` (positive = left louder — a lopsided or collapsed HRTF image), and the
  **mid/side ratio** `sideToMid = RMS(S) − RMS(M)` with `M = (L+R)/√2`, `S = (L−R)/√2` (a very negative
  side/mid ⇒ near-mono collapse; the headphone counterpart to §3c's loudspeaker fold). The
  `add_binaural_monitor` bus is often WIDER than 2 (sized to receive the ambisonic send) with the decoded
  binaural on **channels 1/2** — those two channels are what gets measured (rendered at 2-ch width). Fails
  closed `not_binaural` on a labeled loudspeaker **bed** (use `downmix_check`) or a mono target.
- **Decode coverage (`analysis.decode_coverage`).** The scene is decoded onto a virtual-speaker set — a
  near-uniform **Fibonacci sphere** (default) or a **named delivery layout** (5.1/7.1/7.1.4/9.1.6, nominal
  ITU-R BS.2051 directions; addressed as *directions*, so the renderer-dependent channel interleave order
  is irrelevant and LFE is omitted). Per speaker `s` at direction `d_s`: gain `g_s = Σ_k a_k·Y_k^{SN3D}(d_s)`,
  energy `E_s = ⟨g_s²⟩` (real SN3D SH up to the detected order — higher order ⇒ sharper). Coverage
  statistics are computed on the energy **fractions** `p_s = E_s / Σ E` (level-invariant): **uniformity**
  = normalized Shannon entropy `−Σ p_s ln p_s / ln N` (1 = perfectly even), **CV** = std/mean of the
  fractions, **concentration** = the fraction carried by the loudest ⌈10 %⌉ of directions, **dead zones**
  = directions below `deadZoneFrac · mean` (default 0.1), and the front/back (`d.y ≷ 0`) · left/right
  (`d.x ≶ 0`, left = −x) · upper/lower (`d.z ≷ 0`) · ear-level (`|el| ≤ 15°`) energy balance, plus the
  dominant direction + top hotspots. ⚠️ The **interpretation keys off CV + dead-zones, not the entropy
  uniformity** — a first-order decode is broad, so the normalized entropy compresses near 1 and
  discriminates poorly at low orders, while CV separates a point source (high) from a diffuse field
  (≈ 0) cleanly at every order. Fails closed `not_ambisonic` on a non-ambisonic width (< 4 ch).

## 3e. ADM object-audio authoring (Batch O1)

`spatial.export_adm` authors a native **ITU-R BS.2076** ADM Broadcast-Wave (a DirectSpeakers **bed** +
mono **objects** with position trajectories); `analysis.adm_inspect` parses it back. All the
serialization + parsing is SDK-free in `src/adm_bwf.h` (host-tested `unit.adm`); the tools only render
the stems + sample positions.

- **Container.** `fmt` + `chna` + `axml` + `data`, `data` LAST so the BW64/RF64 (>4 GiB, ITU-R BS.2088)
  path can carry its 64-bit size in `ds64`; classic RIFF/WAVE below 4 GiB (chosen by projected size).
  Chunk allocation `chna` = `uint16 numTracks, numUIDs` + one 40-byte record per data channel
  (`uint16 trackIndex` + `char UID[12]` + `char trackRef[14]` + `char packRef[11]` + pad) — the field
  widths equal the BS.2076 ID string lengths (`ATU_########`, `AT_########_##`, `AP_########`) by design.
- **Coordinate frame** (BS.2076 spherical, matches §3b): azimuth positive **anti-clockwise / to the
  LEFT**, 0° = front; elevation +up; distance 0..1 (1 = room boundary). An object's read-back position
  therefore round-trips its own `spatial.ambisonic_encode`. `coordinateMode:cartesian` maps
  **X = −d·cos(el)·sin(az)** (+right = −left), **Y = d·cos(el)·cos(az)** (+front), **Z = d·sin(el)** (+up).
- **Object trajectory.** Each object = a mono essence channel (its rendered channel 1) + a sequence of
  `audioBlockFormat` blocks on a uniform time grid (`blockMs`, default 100 ms, capped `maxBlocks`), each
  block linearly interpolating the panner's automation. az/el come from the panner's FORMATTED degree
  readout (`GetFXEnvelope` points → `TrackFX_FormatParamValueNormalized` → leading number); distance uses
  the NORMALIZED [0,1] param value directly. A static (un-automated) object = one block; no positional
  panner ⇒ front-center + a warning.
- **Bed.** Authored as a **custom DirectSpeakers** pack with explicit BS.2051 nominal positions +
  `speakerLabel` (M±/U±/B± form) per label — layout-aware (5.1 rear Ls +110 vs 7.1 side Lss +90), LFE
  gets a `<frequency lowPass>`. Custom positions avoid depending on the ITU common-definition pack IDs;
  the Atmos-canonical **7.1.2** bed is supported in addition to the §1 bed layouts.
- **Render substrate.** The bounded bit-exact stem renderers moved from `tools_analysis.cpp` into
  `src/render_stems.h` (shared with the metering path — same RENDER_SETTINGS=3|4 stem-source fix, §M2);
  `export_adm` renders the bed at its width + all objects in one 2-ch multi-stem pass (essence = ch 1).
- **Per-object metadata (Batch O2).** `export_adm`'s `objectMetadata[]` (keyed by `track`) attaches
  three BS.2076 fields per object, written uniformly onto its `audioBlockFormat` blocks (importance onto
  the `audioObject`): **extent** — `<width>/<height>/<depth>` (a `size` 0..1 knob sets all three; or set
  them explicitly — spherical width/height in **degrees**, depth 0..1; cartesian all 0..1); **divergence**
  — `<objectDivergence>` value 0..1 with `azimuthRange` (deg, spherical) or `positionRange` (0..1,
  cartesian), range defaulting 45°/0.5; **importance** — the `importance="0..10"` attribute (a renderer
  may drop lower-importance objects under load). Each is emitted ONLY when set, so a metadata-free object
  is byte-identical to a pre-O2 export. `adm_inspect` reports the footprint back (`hasExtent` /
  `hasObjectDivergence` / `hasImportance` + counts + `objectImportance` values).

## 3f. Dolby Atmos Master ADM Profile conformance (Batch O3)

A generic BS.2076 ADM (O1/O2) is not guaranteed to ingest into a certified Dolby renderer *as an Atmos
master* — that requires the constraints in the **Dolby Atmos Master ADM Profile v1.0**. `export_adm`'s
`profile:"dolby-atmos"` mode authors a conformant file; `analysis.adm_profile_check` reports any ADM
BWF's conformance. The rules + the normalize/validate logic are SDK-free in `src/adm_profile.h`
(host-tested `unit.adm_profile`). We chose the profile (single-file, spec-backed) over authoring the
proprietary **DAMF** triad (`.atmos`/`.atmos.metadata` XML + `.atmos.audio` CAF), which has **no public
spec**; Dolby's own Conversion Tool round-trips a profile-conformant ADM BWF ↔ DAMF.

- **The rules.** Sample rate **48000** (mandatory); the bed is DirectSpeakers **at most 7.1.2** (10 ch —
  a 7.1.4/9.1.6/22.2 "bed" is NOT an Atmos bed; route those channels as objects); **≤128** channels,
  **≤118** objects; single `audioProgramme` `APR_1001`; objects **cartesian**, X/Y/Z ∈ [−1,1]; extent
  **width = depth = height**, all in [0,1]; `objectDivergence` **prohibited**; `importance` only on
  inactive objects (value 0); `interpolationLength` **0 samples** on the first block, **250 samples** on
  every subsequent (carried on `jumpPosition`).
- **Normalize (writer path).** `profile::normalizeModel` bends the model in place — forces cartesian
  (clamping X/Y/Z to [−1,1]), collapses extent to an identical [0,1] size, **strips** the prohibited
  divergence + importance, and sets `Model.dolbyProfile` so the serializer emits the interpolationLength
  ramp. It **fails closed** (returns a `makeError`) on the unfixable invariants — a non-Atmos bed, a
  non-48k render (re-checked against the ACTUAL rendered rate after the render), or the object/channel
  caps — because those can't be coerced without misrepresenting the material. Every auto-normalization is
  echoed in the tool's `profile.normalized[]`.
- **Validate (read path).** `profile::validateParsed` scans a parsed ADM (chna summary + raw `axml`,
  now exposed on `ParseResult`) and returns `conformant` + a `violations[]` list of `{code, detail}`
  (`sample_rate` / `bed_layout` / `channel_cap` / `object_cap` / `programme_id` / `coordinate` /
  `extent` / `divergence` / `importance` / `interpolation`). Point it at our output or a third-party
  (Nuendo / Pro Tools / Dolby) file to QC before delivery.
- **Honesty caveat** (same footing as O1/O2): the output is profile-**shaped** and self-validated here;
  whether a SPECIFIC certified renderer ingests it exactly is the live-gate / ingest check on real tools.
- **Backward-compatible:** without `profile:"dolby-atmos"`, `export_adm` is byte-identical to O1/O2
  (`Model.dolbyProfile` defaults false; no interpolationLength, no clamp).

## 4. Coordinate convention (azimuth / elevation → position)

Angles are in **degrees**. The tools convert to a unit direction vector with **x = right, y = front,
z = up**:

```
az_rad = azimuthDeg   × π/180
el_rad = elevationDeg × π/180

x = -sin(az) · cos(el)    # +x = right, so +az points left
y = cos(az) · cos(el)     # front
z = sin(el)               # up
```

So **azimuth 0° = front** (x = 0, y = 1); **+90° = hard left** (x = −1, y = 0); azimuth increases
counter-clockwise viewed from above. **Elevation +90° = directly overhead** (z = 1); 0° = ear-level.

For `spatial.set_source_position` into ReaSurroundPan, `x`/`y` are accepted directly in **`[-1, 1]`**
(with optional `z` height) or as `azimuthDeg`/`elevationDeg`. Whatever the input, values are written to
the panner **normalized to `[0, 1]` with centre 0.5** (`n = (v + 1) / 2`), so any underlying param range
is safe. Param indices for the X/Y/Z axes are discovered by name, with explicit `paramX/paramY/paramZ`
overrides.

> **Consistent azimuth.** This `+az = left` frame is shared by `spatial.set_source_position` / `spatialize_stems` (the surround panner), `spatial.ambisonic_encode` (§3), the ADM writer (§3e), and the metering read-back (§3b), so a source described at a given azimuth lands on the same physical side in all of them. ReaSurroundPan's `in 1 X` param runs **0.0 = right → 1.0 = left** (the reverse of this x = right frame; live-verified 2026-07-10), so the panner tools negate x when writing in1X.

Semantic placement descriptors (e.g. `front-center`, `wide`, `overhead`) resolve to `{az, el, width,
divergence, lfe}` defaults in `placement_defaults`; explicit numeric `az`/`el` always override a
descriptor.

## 5. Loudness / deliverable specs

`analysis.check_deliverable` compares a measured master's integrated LUFS, true peak, and (where
applicable) short-term max against a **named** spec and returns per-metric pass/fail. LUFS values are
LUFS-I; the true-peak column is a dBTP ceiling (measured ≤ limit). Aliases resolve case-insensitively.

| Spec (aliases) | Integrated target | Tolerance | True-peak ceiling | Layout(s) | Use |
| --- | --- | --- | --- | --- | --- |
| `atmos-music` | −18 LUFS-I | ±1 LU | −1 dBTP | 7.1.4 bed + objects | Apple / Dolby streaming immersive |
| `streaming-stereo` | −14 | ±1 LU | −1 | 2.0 | Spotify / YouTube / Tidal / Amazon |
| `apple-music-stereo` | −16 | ±1 LU | −1 | 2.0 | Apple Music (Sound Check) |
| `ebu-r128` (`broadcast-r128`) | −23 | ±0.5 LU (±1 live) | −1 | 2.0 / 5.1 | EBU broadcast delivery |
| `ebu-r128-s1` | −23 target; **Max short-term ≤ −18 LUFS** | ±0.5 LU | −1 | 2.0 | EBU short-form (adverts / promos ≲ 2 min) |
| `atsc-a85` (`calm-act`) | −24 LKFS | ±2 LK | −2 | 2.0 / 5.1 | US digital TV / CALM Act (dialog-anchored) |
| `podcast` | −16 (−19 **mono**) | ±1 LU | −1 | mono / 2.0 | Spoken word (Apple Podcasts) |
| `cinema-theatrical` (`theatrical`) | **SPL-referenced — not LUFS-gated** | — | −3 | 5.1 / 7.1 | Dubbing-stage 85 dB SPL reference |

Notes that matter in practice:

- **`ebu-r128-s1`** gates on **Max short-term (≤ −18 LUFS)** rather than integrated loudness, because
  integrated gating is unreliable on programme under ~2 minutes.
- **`atsc-a85`** allows **−2 dBTP** (looser than EBU's −1), leaving headroom for lossy broadcast codecs.
- **`podcast`** uses a distinct **−19 LUFS** target when the render is mono (1 channel), else −16.
- **`cinema-theatrical`** is monitored at a fixed **85 dB SPL** (pink-noise-calibrated), so a LUFS
  pass/fail is meaningless — LUFS is reported but not gated; the true-peak ceiling (−3 dBTP) still gates.

Overall pass is only meaningful when at least one metric was actually gated; an ungated report (e.g. a
silent test bed, or a spec with no gated metric measured) returns `pass: null`.

---

_Conventions verified against `tools_spatial.cpp`, `spatial_verbs.h`, `deliverable_specs.h`,
(§3b/§3c/§3d) `ambisonic_meter.h`, and (§3e/§3f) `adm_bwf.h`/`adm_profile.h` on 2026-07-09 (Batch O3).
Regenerate `docs/REFERENCE.md` and re-check this document after any change to the bed table, ambisonic
wiring, coordinate math, the metering math, the ADM/profile logic, or the deliverable-spec table._
