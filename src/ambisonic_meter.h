// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// ambisonic_meter.h — SDK-free, JSON-free immersive-metering DSP.
//
// The measurement engine behind analysis.meter and analysis.spatial_field. Everything here is pure
// C++/std operating on a plain interleaved float buffer (an AudioBuffer), so the whole DSP is
// host-unit-testable with synthetic signals — no REAPER, no JSON, no SWELL (exactly like track_chunk.h,
// deliverable_specs.h, actions_policy.h). The tool layer (tools_analysis.cpp) renders a bounded, non-
// destructive stem via the render_stats.h machinery, reads the resulting WAV with readWavFile(), and
// hands the buffer to these analyzers. Program loudness (integrated LUFS / LRA / true-peak) stays with
// REAPER's native RENDER_STATS; this header ADDS what RENDER_STATS cannot give: per-channel
// level/peak/true-peak/K-weighted level, inter-channel correlation / downmix compatibility, BS.1770-4
// gated stem/dialog loudness and a momentary/short-term loudness timeline, the ambisonic spatial field
// (DirAC intensity direction-of-arrival + diffuseness, and the Gerzon energy / velocity localization
// vectors rE / rV), and ambisonic decode-coverage statistics.
//
// Conventions (docs/CONVENTIONS.md §3/§3a/§3b, kept byte-consistent with the encoders):
//   * ACN channel k has degree l = floor(sqrt(k)), order m = k - l*l - l  (m in [-l,+l]).
//   * Normalization SN3D (default; N3D accepted). In SN3D the first-order dipoles equal the direction
//     cosines, so a plane wave gives W=s and [ACN3,ACN1,ACN2] = s*[front,left,up] (ambiX axes).
//   * REPORTED azimuth is the ambisonic-standard convention: measured CCW from front, +az = LEFT
//     (ACN/SN3D/ambiX, as used by IEM/SPARTA and spatial.ambisonic_encode). Elevation +90 = overhead.
//     A source spatial.ambisonic_encode places at azimuth theta reads back at DoA azimuth theta.
//     NB this differs from the ReaSurroundPan §4 channel-panner frame (x=right, +az=right); the field
//     analyzer measures an *ambisonic* bus, so it speaks the ambisonic azimuth convention.
//   * Internally the intensity/rV/rE vectors are accumulated in a right-handed (x=right, y=front, z=up)
//     frame; vecToAzEl() converts to the reported ambisonic azimuth (left = -x) at the boundary.

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace reaper_mcp {
namespace meter {

// ------------------------------------------------------------------------------------------------
// Audio buffer + small numeric helpers
// ------------------------------------------------------------------------------------------------

struct AudioBuffer {
    int channels = 0;
    double sampleRate = 0.0;
    size_t frames = 0;
    std::vector<float> samples;  // interleaved, size == frames * channels

    inline float at(size_t frame, int ch) const { return samples[frame * (size_t)channels + ch]; }
};

inline double kMinDb() { return -144.0; }
inline double linToDb(double x) { return x > 1e-12 ? 20.0 * std::log10(x) : kMinDb(); }
inline double powToDb(double p) { return p > 1e-24 ? 10.0 * std::log10(p) : kMinDb(); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double rad2deg(double r) { return r * (180.0 / M_PI); }

// ------------------------------------------------------------------------------------------------
// Minimal WAV reader — RIFF/WAVE, PCM 16/24/32-bit int + IEEE float 32/64, arbitrary channel count.
// Returns interleaved float32 in AudioBuffer. Host-side (tool layer) use only; the DSP below never
// touches the filesystem, so unit tests build buffers directly.
// ------------------------------------------------------------------------------------------------

namespace detail {
inline uint32_t rdU32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rdU16(const unsigned char* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }
}  // namespace detail

inline bool readWavFile(const std::string& path, AudioBuffer& out, std::string& err) {
    out = AudioBuffer{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open file: " + path; return false; }
    std::vector<unsigned char> hdr(12);
    if (std::fread(hdr.data(), 1, 12, f) != 12 ||
        std::memcmp(hdr.data(), "RIFF", 4) != 0 || std::memcmp(hdr.data() + 8, "WAVE", 4) != 0) {
        std::fclose(f); err = "not a RIFF/WAVE file"; return false;
    }
    uint16_t fmtTag = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    bool haveFmt = false;
    std::vector<unsigned char> data;
    unsigned char ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        uint32_t sz = detail::rdU32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<unsigned char> fb(sz);
            if (std::fread(fb.data(), 1, sz, f) != sz) break;
            if (sz >= 16) {
                fmtTag = detail::rdU16(fb.data());
                channels = detail::rdU16(fb.data() + 2);
                sampleRate = detail::rdU32(fb.data() + 4);
                bits = detail::rdU16(fb.data() + 14);
                // WAVE_FORMAT_EXTENSIBLE (0xFFFE): the real tag is the first 2 bytes of the GUID subformat.
                if (fmtTag == 0xFFFE && sz >= 26) fmtTag = detail::rdU16(fb.data() + 24);
                haveFmt = true;
            }
            if (sz & 1) std::fseek(f, 1, SEEK_CUR);  // chunks are word-aligned
        } else if (std::memcmp(ch, "data", 4) == 0) {
            data.resize(sz);
            if (std::fread(data.data(), 1, sz, f) != sz) { data.clear(); break; }
            if (sz & 1) std::fseek(f, 1, SEEK_CUR);
        } else {
            std::fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);  // skip unknown chunk (word-aligned)
        }
    }
    std::fclose(f);
    if (!haveFmt || channels == 0) { err = "missing/invalid fmt chunk"; return false; }
    if (data.empty()) { err = "no data chunk / empty audio"; return false; }

    const int bytes = bits / 8;
    if (bytes == 0) { err = "invalid bit depth"; return false; }
    const size_t totalSamples = data.size() / (size_t)bytes;
    const size_t frames = totalSamples / channels;
    out.channels = channels;
    out.sampleRate = (double)sampleRate;
    out.frames = frames;
    out.samples.resize(frames * (size_t)channels);

    const unsigned char* p = data.data();
    const bool isFloat = (fmtTag == 3);  // WAVE_FORMAT_IEEE_FLOAT
    for (size_t i = 0; i < frames * (size_t)channels; ++i) {
        const unsigned char* s = p + i * (size_t)bytes;
        float v = 0.0f;
        if (isFloat) {
            if (bits == 32) { float t; std::memcpy(&t, s, 4); v = t; }
            else if (bits == 64) { double t; std::memcpy(&t, s, 8); v = (float)t; }
        } else {
            if (bits == 16) { int16_t t = (int16_t)detail::rdU16(s); v = (float)(t / 32768.0); }
            else if (bits == 24) {
                int32_t t = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16));
                if (t & 0x800000) t |= (int32_t)0xFF000000;  // sign-extend 24->32
                v = (float)(t / 8388608.0);
            } else if (bits == 32) {
                int32_t t = (int32_t)detail::rdU32(s); v = (float)(t / 2147483648.0);
            }
        }
        out.samples[i] = v;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Biquad (Transposed Direct Form II) + the ITU-R BS.1770 K-weighting filter (design at any sample rate)
// ------------------------------------------------------------------------------------------------

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalized to 1
    // Process in place across one channel's samples.
    inline void process(std::vector<double>& x) const {
        double z1 = 0, z2 = 0;
        for (double& v : x) {
            double in = v;
            double out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            v = out;
        }
    }
};

// BS.1770 stage 1: a high-frequency shelving boost modelling the head; stage 2: a high-pass (RLB).
// Designed from the ITU analog prototype via the bilinear transform at the given sample rate, so it is
// correct at 44.1/48/96 kHz etc. (the fixed-48k coefficient tables are just this evaluated at 48k).
// K-weighting biquads use the ITU-R BS.1770 pre-warped analog-prototype coefficients as in common
// reference implementations (e.g. libebur128 / pyloudnorm).
inline Biquad k1ShelfBiquad(double fs) {
    const double f0 = 1681.9744509555319;
    // Historically this was 3.99984385397 (12 significant digits). The reference value carries
    // 16, and the truncation propagated into b1 = 2(K^2 - Vh)/a0 as a 1.044942e-12 error
    // against Table 1 -- which is exactly the "1.045e-12 shelf agreement" recorded earlier
    // and attributed to the TABLE's own precision. It was ours, not the table's.
    const double G = 3.999843853973347;   // dB

    const double Q = 0.7071752369554193;
    const double K = std::tan(M_PI * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    Biquad bq;
    bq.b0 = (Vh + Vb * K / Q + K * K) / a0;
    bq.b1 = 2.0 * (K * K - Vh) / a0;
    bq.b2 = (Vh - Vb * K / Q + K * K) / a0;
    bq.a1 = 2.0 * (K * K - 1.0) / a0;
    bq.a2 = (1.0 - K / Q + K * K) / a0;
    return bq;
}
inline Biquad k2HighpassBiquad(double fs) {
    const double f0 = 38.13547087602444;
    const double Q = 0.5003270373238773;
    const double K = std::tan(M_PI * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    Biquad bq;
    // BS.1770-4 Table 1 keeps the RLB numerator as exactly [1, -2, 1] — it is NOT divided
    // by a0, unlike the shelf's. Dividing it (as this once did) forces the
    // passband gain to exactly 1.0, while the tabulated filter's is a0 (1.004994898715 at
    // 48 kHz). The -0.691 offset is calibrated against the TABULATED filter, so the
    // normalised form reads low by 20*log10(a0): 0.047099 / 0.043277 / 0.023566 /
    // 0.021652 LU at 44.1 / 48 / 88.2 / 96 kHz. Verified against Table 1 in
    // unit.meter "RLB numerator matches BS.1770-4 Table 1".
    bq.b0 = 1.0;
    bq.b1 = -2.0;
    bq.b2 = 1.0;
    bq.a1 = 2.0 * (K * K - 1.0) / a0;
    bq.a2 = (1.0 - K / Q + K * K) / a0;
    return bq;
}

// Mean square of one channel after K-weighting (the per-channel building block of BS.1770 loudness).
inline double kWeightedMeanSquare(const AudioBuffer& buf, int ch) {
    if (buf.frames == 0) return 0.0;
    std::vector<double> x(buf.frames);
    for (size_t i = 0; i < buf.frames; ++i) x[i] = buf.at(i, ch);
    k1ShelfBiquad(buf.sampleRate).process(x);
    k2HighpassBiquad(buf.sampleRate).process(x);
    double sum = 0.0;
    for (double v : x) sum += v * v;
    return sum / (double)buf.frames;
}

// ------------------------------------------------------------------------------------------------
// True peak (dBTP) — 8x oversampling with a 24-tap-per-phase polyphase FIR OF OUR OWN DESIGN.
// ------------------------------------------------------------------------------------------------
//
// ⛔ THIS IS A CLAIM CHANGE (the release after 1.20.0).  Through 1.20.0 the interpolator was ITU-R BS.1770-4 Annex 2's
//   own tabulated 48-tap, 4-phase filter, coefficient for coefficient, at the 4x the Recommendation
//   sets as its minimum.  Measured in context against an exact (FFT-domain) reference on the EBU's
//   published programme material, that estimator read up to 0.52 dB LOW (SQAM 27, castanets) and up
//   to 0.22 dB HIGH (Euroradio 05), and the shape of the error was diagnosed: at 4x the evaluation
//   GRID under-reads by up to 20*log10(cos(pi*f/4)) -- 0.44 dB at 0.40 fs -- and the 12-tap table's
//   passband ripple (+0.22 dB at fs/4) happened to cancel some of it on some material while adding to
//   it on other material.  A better filter at 4x reads WORSE (0.25 dB), because an accurate filter
//   simply exposes the grid.  The design point is joint: grid AND filter.
//
// WHAT SHIPS.  Eight phases at delays (2q+1)/16 of a sample (none of them the input sample, which is
//   kept as a floor), each a 24-tap per-phase minimax design with a passband edge at 0.45 fs, every
//   phase DC-normalised, mirror-symmetric (q <-> 7-q).  Computed passband complex error 0.00996
//   (+0.086 / -0.087 dB), no gain above the edge.  ⛔ THE TABLE BELOW IS THE DESIGN, NOT A DERIVATION
//   OF IT: it was designed once (Lawson IRLS, a fixed point rather than a proof of Chebyshev
//   optimality), evaluated against the reference, and written down.  Change a coefficient and every
//   disclosed bound in this header follows it, because they are computed from the table at first call.
//
// WHAT IT BUYS, MEASURED THE SAME WAY ON THE SAME MATERIAL (every window in its file context, against
//   the exact reference): worst error 0.137 dB LOW (castanets) from 0.522, and up to 0.097 dB HIGH
//   (SQAM 64) from 0.216; on the standard's own true-peak test signals the worst is 0.127 dB low at
//   fs/4 (cases 16 and 19), every one inside the +0.2/-0.4 dB window EBU Tech 3341 sets.  The 0.137 is
//   the 8x GRID's own bound at the passband edge -- 20*log10(cos(pi*0.45/8)) = 0.1363 -- which is
//   what an accurate filter leaves.  16x buys 0.016 more; 32 taps buys 0.010; both at once, 0.03.
//
// WHAT IT COSTS, STRUCTURALLY: 8 phases x 24 taps = 192 multiply-adds per input sample, against the
//   4x12 table's 48 -- four times the interpolation work (the meter runs ONE pass for both true-peak
//   numbers, see truePeakLinearFused).  A timed multiplier is a separate measurement on a quiet
//   machine and is not written here.
//
// The previous interpolator's own predecessor was a 12-tap Hann-windowed sinc with the input sample
// as phase 0; it drooped -0.9 dB at 0.40 fs and always read low.  The Recommendation's table fixed the
// droop and left the grid; this one addresses both.
inline std::vector<std::vector<double>> truePeakPolyphase() {
    // Phase q (row), tap t (column).  Delay of phase q: (2q+1)/16 sample.  Output position i reads
    // input samples i-11 .. i+12 (idx = i + t - 11).  17 significant digits: the design as computed.
    static const double k[8][24] = {
        { -0.0015819068091455616, 0.0016992223948436101, -0.0022698332128575564, 0.0034681277171338424, -0.0046147363786762105, 0.0065513254236299265,
          -0.0087938067053162233, 0.012431580265103626, -0.017780833199325143, 0.028335208466205561, -0.057125652745007457, 0.99304860904026004,
          0.066287321914134889, -0.031312985954301188, 0.019937746576560101, -0.013834385254721623, 0.010260497867554969, -0.0075066122118163919,
          0.0057492786570788532, -0.0041146059894919323, 0.0031458400952364108, -0.0020770827820778661, 0.0015740624243883701, -0.001476379599393157},
        { -0.0044556152824453688, 0.0047798126337519611, -0.0063728492023551646, 0.0097238136155321751, -0.012904111107786704, 0.018272642670042114,
          -0.024418742621230376, 0.034326289399373336, -0.048603166511983931, 0.076027541552725675, -0.14539934304659619, 0.94187182780000578,
          0.21755211480763717, -0.095231249991409819, 0.059223353953998165, -0.040645587927165223, 0.02993497246871249, -0.021811181910731066,
          0.016646399820315565, -0.011889480391252658, 0.0090697099410550208, -0.0059815523338887658, 0.004525662481026883, -0.0042412608173309201},
        { -0.006597880990558312, 0.007069299990151442, -0.009408058748187207, 0.014335860596895426, -0.018975306416193854, 0.026803908653684635,
          -0.035667395728629765, 0.049872785831657551, -0.06995129585790455, 0.10759350555251421, -0.19673583632785116, 0.84534950759265071,
          0.38445872906298562, -0.15298746070991828, 0.092665716525959257, -0.062847007216821113, 0.045944333860192026, -0.033332291356168255,
          0.02534670533940651, -0.018065808004438119, 0.013749371751880662, -0.0090571518743041649, 0.0068413428586704926, -0.0064055743856740064},
        { -0.0077034650970589733, 0.0082444427338694455, -0.010952339420182712, 0.016667461968259911, -0.022005967807104718, 0.031011907216851254,
          -0.041098444859379371, 0.057178195535271023, -0.0794899199776056, 0.12040319906915967, -0.21180114503096248, 0.71214446215754545,
          0.5539905068921992, -0.19484461178913512, 0.11456501131114004, -0.076704621017147825, 0.05563644700628171, -0.040181548539550672,
          0.030439552403556714, -0.021648672995547576, 0.016437029148536373, -0.010814455775224021, 0.008154862278685426, -0.0076278854124571093},
        { -0.0076278854124575447, 0.0081548622786858961, -0.010814455775224226, 0.016437029148536429, -0.021648672995547406, 0.030439552403558195,
          -0.040181548539551068, 0.055636447006281446, -0.076704621017148686, 0.11456501131114001, -0.19484461178913648, 0.55399050689219986,
          0.7121444621575449, -0.21180114503096273, 0.12040319906915865, -0.079489919977604157, 0.057178195535271065, -0.041098444859378157,
          0.031011907216851813, -0.022005967807105144, 0.016667461968257927, -0.010952339420182462, 0.0082444427338701758, -0.0077034650970583029},
        { -0.0064055743856729084, 0.006841342858668901, -0.0090571518743061373, 0.013749371751880249, -0.01806580800443645, 0.025346705339407964,
          -0.033332291356167151, 0.04594433386019324, -0.062847007216820475, 0.092665716525958869, -0.1529874607099185, 0.3844587290629865,
          0.84534950759264982, -0.19673583632785169, 0.10759350555251393, -0.069951295857903148, 0.049872785831659827, -0.035667395728627024,
          0.026803908653683827, -0.018975306416195815, 0.014335860596892964, -0.0094080587481886902, 0.0070692999901493464, -0.0065978809905575366},
        { -0.0042412608173295289, 0.0045256624810272924, -0.0059815523338883261, 0.0090697099410546409, -0.011889480391252732, 0.016646399820312859,
          -0.021811181910732103, 0.029934972468712125, -0.040645587927163239, 0.059223353954001128, -0.095231249991408126, 0.21755211480763459,
          0.94187182780000434, -0.14539934304659782, 0.076027541552724551, -0.048603166511985145, 0.034326289399373468, -0.024418742621230678,
          0.018272642670041382, -0.012904111107788128, 0.0097238136155324804, -0.0063728492023527941, 0.0047798126337542275, -0.0044556152824443565},
        { -0.0014763795993944962, 0.0015740624243900099, -0.0020770827820787057, 0.0031458400952376463, -0.0041146059894924944, 0.0057492786570828838,
          -0.0075066122118171257, 0.010260497867558114, -0.01383438525472353, 0.019937746576564636, -0.031312985954300834, 0.066287321914137207,
          0.99304860904025083, -0.057125652745011149, 0.028335208466201404, -0.017780833199321014, 0.012431580265103121, -0.0087938067053140133,
          0.0065513254236261005, -0.0046147363786769148, 0.0034681277171312651, -0.0022698332128554977, 0.0016992223948448686, -0.0015819068091422366},
    };
    std::vector<std::vector<double>> ph(8, std::vector<double>(24, 0.0));
    for (int p = 0; p < 8; ++p)
        for (int t = 0; t < 24; ++t) ph[p][t] = k[p][t];
    return ph;
}

// The edge guard, in SAMPLES, excluded at EACH end by truePeakDbInterior below.
//
// ⛔ TWELVE, AND IT IS DERIVED FROM THE FILTER RATHER THAN SWEPT.  An output at position i reads
//    idx = i + (t - half + 1) for t in [0, taps), i.e. i-11 .. i+12.  Zero-padding therefore reaches
//    positions 0..10 at the head and the last TWELVE positions at the tail, so 12 is the smallest guard
//    that covers every position the padding can touch -- AT ANY BUFFER LENGTH, FOR ANY SIGNAL.
//    (Through 1.20.0 the table had 12 taps and the guard was 6, derived the same way.)
//    Change the table and this constant follows it: kTruePeakEdgeGuard IS kTruePeakTaps / 2, and
//    truePeakLinearOver ASSERTS the table really has that many taps, so the two cannot drift.
//
// ⛔ AND IT CONTRADICTS AN EARLIER CORRECTION, WHICH SAID "the guard must be 2, not 8".
//    That figure came from sweeping ONE synthetic sine of period 8 samples, whose ring happens to
//    collapse within two positions.  The residual was then measured THROUGH THE PRODUCT on real
//    material (~439.5 Hz, period ~109 samples) across eight 1 ms window positions and read:
//        guard 2 -> +0.115617 dB of the artefact LEFT BEHIND    guard 4 -> +0.041171
//        guard 5, 6, 7, 8 -> +0.000000
//    ⇒ a guard of 2 SHIPS THE DEFECT IT WAS ADDED TO DISCLOSE, on ordinary material.  The earlier
//    "generous 8" was closer to right than the beat that corrected it, and the derivation above
//    is why the guard is the tap geometry and not a round number (6 for that table; 12 for this one).
//
// ⛔ IT IS A CONSTANT AND NOT A PARAMETER, on purpose: a flag that changes what "true peak" means
//    is that same class of defect waiting to happen.
inline constexpr int kTruePeakTaps      = 24;   // per phase; the table above IS the design
inline constexpr int kTruePeakEdgeGuard = kTruePeakTaps / 2;

// The oversampling factor, and THE CEILING IT PUTS ON THIS METER'S HONESTY.
//
// WHY THIS EXISTS.  An 8x estimator evaluates the reconstructed waveform only on a grid of 1/8
//   sample, so the nearest evaluation point to an actual inter-sample maximum is at most 1/16 of a
//   sample away.  For a sinusoid at frequency f the value there is cos(2*pi*f*dt) times the peak,
//   with dt = 1/(2*OS) samples -- so the reading can fall BELOW the truth, and the shortfall grows
//   with frequency.  At the band edge that is -20*log10(cos(pi/(2*OS))).
//   ⛔ IT ERRS IN THE UNSAFE DIRECTION: it reports headroom that is not there.  A meter whose whole
//   job is to be trusted at the ceiling should say so rather than let the number stand alone --
//   the same argument that put the edge-guard reading beside the number it explains.
//
// ⛔ WHAT THE BOUND COVERS AND WHAT IT DOES NOT, because a bound whose scope is not stated is a
//   claim rather than a measurement:
//     it covers  -- the EVALUATION GRID: the estimator simply does not look between its phases.
//     it EXCLUDES -- the interpolation filter's own magnitude error at the phase it does evaluate,
//                    which is a separate quantity of a different sign convention and is not
//                    modelled here.  ⇒ THE TOTAL SHORTFALL CAN EXCEED THIS NUMBER.  The field is
//                    named "grid" for that reason and the tool description says it in words.
//     it assumes  -- a sinusoid FOR ITS WORST CASE, and for THIS field the assumption turns out to be
//                    unnecessary: a classical inequality for band-limited functions (Duffin and
//                    Schaeffer's, through its cosine corollary) gives f(t0 + d) >= M cos(2 pi W d) for
//                    EVERY real signal band-limited to W, so the EXACT value at the nearest lattice
//                    instant is within this bound of the peak for any such signal, with the edge sine
//                    as the extremal case.  The grid half of the guarantee is therefore a theorem and
//                    not an extrapolation from sines.
//                    THE SAME IS NOT TRUE OF THE FILTER -- see truePeakSineLowBoundDb's scope below.
//
// ⚠️ THE MEASURED LOW SIDE, FOR THIS TABLE, in context against an exact reference on the EBU's
//   published files: castanets (SQAM 27, the brightest file) reads 0.136516 dB LOW -- and the grid
//   bound at the passband edge, 20*log10(cos(pi*0.45/8)) = 0.1363, IS that number: the grid is what
//   an accurate filter leaves.  The standard's fs/4 tones (cases 16, 19) read 0.1266 dB low, which is
//   the fs/4 grid figure (0.0419) plus the phase ripple (up to 0.087 dB).  The total LOW side is
//   therefore bounded by this grid bound PLUS the filter's per-phase loss, and the loss is disclosed
//   in words in the tool description; the HIGH side is truePeakFilterGainBoundDb, below.
//   ⛔ SUPERSEDED IN PART: the loss is now ALSO a field, truePeakFilterLossBoundDb, computed
//   from the table like this one -- the sentence above is kept for the trail.
// ⛔ THE 4x TABLE'S FIGURES, KEPT FOR THE TRAIL: it read case 17 0.2953 dB low, castanets 0.521751 dB
//   low and Euroradio 05 +0.215802 dB high; its band-edge grid bound was 0.6877 dB and its fs/4 grid
//   figure 0.1685.  Those numbers describe an estimator that no longer ships.
//
// ⛔ THE FACTOR WAS RAISED TOGETHER WITH THE FILTER, NOT ALONE.  Raising it alone buys little (12 taps
//   is the filter's limit) and a better filter alone reads worse (it exposes the 4x grid); the two
//   move together, and the cost -- four times the interpolation work -- is stated above.
inline constexpr int kTruePeakOversampling = 8;   // BS.1770-4 Annex 2 sets 4x as the MINIMUM; this is 8x

inline double truePeakGridBoundDb() {
    // ⛔ COMPUTED, NEVER A LITERAL.  Change kTruePeakOversampling and this follows it; a hardcoded
    //   0.1685 would quietly describe a meter that no longer exists (0.6877 already did, at 4x).
    return -20.0 * std::log10(std::cos(M_PI / (2.0 * (double)kTruePeakOversampling)));
}

// THE OTHER SIDE OF THE BOUND: the interpolation filter's own GAIN ABOVE UNITY.
//
// WHY THIS EXISTS.  truePeakGridBoundDb above bounds one direction only -- how far the GRID can read
//   a sine LOW -- and it declared the filter's magnitude error as an exclusion.  That exclusion was
//   then measured: against an exact (FFT-domain) reference, with every window read in its file
//   context, the shipped table reads the EBU's Euroradio programme material up to +0.215802 dB HIGH.
//   An over-read has exactly one source in this estimator: a phase whose gain exceeds 1 at the
//   signal's frequency.  So the over-read bound for a sine is the largest |H_p(f)| over the four
//   phases and all frequencies -- a number that follows from the twelve tabulated coefficients and
//   nothing else.  For the table that ships it is +0.086 dB, attained low in the band (near 0.037 fs)
//   where the minimax ripple peaks; for the 4x table it was +0.22 dB near fs/4.  A reading can
//   therefore sit ABOVE the truth by up to this much:
//       truth - (grid bound + filter loss)  <=  truePeakDb  <=  truth + truePeakFilterGainBoundDb.
//
// ⛔ WHAT IT COVERS AND WHAT IT DOES NOT:
//     it covers  -- the FILTER's gain above unity at the phases the estimator evaluates, for a sine.
//                   It is an OVER-read bound; a reading above it is a defect in this code.
//     it EXCLUDES -- the filter's LOSS (gain below unity), which adds to the LOW side beyond the
//                    grid bound and is NOT bounded by any field: this table's per-phase ripple is
//                    up to 0.087 dB, so the standard's fs/4 tones read 0.1266 dB low where the grid
//                    alone accounts for 0.0419, and castanets reads 0.1365 dB low at the passband
//                    edge where the grid alone accounts for 0.1363.
//                    ⛔ "NOT bounded by any field" IS SUPERSEDED: it is now bounded by
//                    truePeakFilterLossBoundDb, below; the exclusion from THIS field stands.
//     it assumes  -- a sinusoid, like its sibling; on a broadband transient the phases' phase error
//                    can align components better than the input had them, and THIS table was measured
//                    over-reading a real window (SQAM 64) by +0.0968 dB, 0.011 dB MORE than its sine
//                    bound.  The unit test asserts that excess exists and that it is small.
//     it applies to -- the INTERIOR reading (truePeakDbInterior).  truePeakDb is the whole-buffer
//                    maximum and its taps are fed zeros beyond the buffer; at a raw edge the step
//                    they ring at can exceed the filter's gain on the signal (measured at
//                    up to 0.94 dB on one steady tone).  truePeakEdgeDominated is the flag; the unit
//                    test asserts both halves.  ⚠️ The first build of that test asserted the bound
//                    through truePeakDb() and FAILED -- correctly.
//
// ⛔ COMPUTED FROM THE TABLE AT FIRST CALL, NEVER A LITERAL.  Change one coefficient and this follows
//   it; a hardcoded 0.086 would describe a table that no longer exists (0.222 already does, the 4x
//   table's, and it was never a literal either -- the same argument as above,
//   and the unit test asserts the bound is TIGHT against the meter itself, which a stale literal
//   would fail).  The scan is 8193 points on [0, fs/2] plus a golden-section refinement; the cost
//   is paid once per process.
inline double truePeakFilterGainBoundDb() {
    static const double bound = [] {
        const std::vector<std::vector<double>> ph = truePeakPolyphase();
        const int taps = (int)ph[0].size(), half = taps / 2;
        auto gainAt = [&](double f) {
            const double w = 2.0 * M_PI * f; double g = 0.0;
            for (const auto& hp : ph) {
                double re = 0.0, im = 0.0;
                for (int t = 0; t < taps; ++t) { const double m = (double)(t - half + 1); re += hp[t] * std::cos(w * m); im += hp[t] * std::sin(w * m); }
                const double a = std::sqrt(re * re + im * im); if (a > g) g = a;
            }
            return g;
        };
        const int N = 8192; double best = 0.0, fb = 0.0;
        for (int k = 0; k <= N; ++k) { const double f = 0.5 * (double)k / N; const double g = gainAt(f); if (g > best) { best = g; fb = f; } }
        double lo = fb - 0.5 / N, hi = fb + 0.5 / N; if (lo < 0.0) lo = 0.0; if (hi > 0.5) hi = 0.5;
        for (int it = 0; it < 80; ++it) {
            const double a = hi - 0.618033988749895 * (hi - lo), b = lo + 0.618033988749895 * (hi - lo);
            if (gainAt(a) < gainAt(b)) lo = a; else hi = b;
        }
        return 20.0 * std::log10(gainAt(0.5 * (lo + hi)));
    }();
    return bound;
}

// THE THIRD NUMBER: the interpolation filter's own LOSS below unity, within its passband.
//
// WHY THIS EXISTS.  The two bounds above leave one side half-stated: truePeakGridBoundDb says how far
//   the GRID can read a sine low and EXCLUDES the filter's loss; truePeakFilterGainBoundDb says how
//   far the FILTER can read high.  How far the filter can read LOW was a sentence in the description
//   ("up to 0.09 dB per phase").  A sentence goes stale the day the table changes; a number computed
//   from the table does not.  The loss bound is the largest attenuation of any phase at any frequency
//   inside the passband -- max over q and f in [0, kTruePeakPassbandEdge] of -20 log10 |H_q(f)| --
//   so that for a sine inside the passband:
//       truth - (truePeakGridBoundDb + truePeakFilterLossBoundDb)  <=  truePeakDbInterior  <=  truth + truePeakFilterGainBoundDb.
//   For the table that ships it is about 0.088 dB, attained AT the passband edge (phases 3 and 4, the
//   two farthest from an input sample); the same phases lose 0.084 dB at fs/4, which with the fs/4
//   grid figure (0.042) is within 0.001 dB of the 0.1266 the standard's fs/4 tones measure low.  The
//   unit test drives a sine at fs/4 through the meter and asserts the reading is within 0.002 dB of
//   grid + loss at that frequency (a long tone at fs/4 samples the same four positions for ever, so no
//   later cycle rescues it).  ⛔ WITHIN, NOT "NEVER BELOW": the meter reads 0.0008 dB LOWER than grid +
//   loss there, because grid + loss is an ideal-delay model and the filter's PHASE error moves each
//   phase's effective sampling instant -- a THIRD low-side contribution, not disclosed as a field (it is
//   0.0008 dB at fs/4 for this table).  The sum of the two fields still covers the true single-peak
//   worst case for a sine anywhere in the passband (0.127 dB, at fs/4, for this table), because the grid
//   bound is taken at fs/2 and the loss bound at the passband edge, where no single peak meets both.
//   ⛔ SUPERSEDED IN PART: that worst case is now ALSO a field, truePeakSineLowBoundDb, computed
//   from the table like this one, with the phase error in -- the figure above is kept for the trail.
//
// ⛔ WHAT IT COVERS AND WHAT IT DOES NOT:
//     it covers   -- the FILTER's attenuation at the phases the estimator evaluates, for a sine INSIDE
//                    the passband.  It is a LOW-side quantity like the grid bound and ADDS to it; the
//                    sum is the low-side bound -- per frequency in the inequality above and, as the two
//                    fields, a frequency-independent worst case.
//     it EXCLUDES -- content ABOVE the passband edge (kTruePeakPassbandEdge, 0.45 fs: 21.6 kHz at
//                    48 kHz, 19.8 kHz at 44.1 kHz), where the filter rolls off by design (about 14 dB
//                    at fs/2).  On band-limited programme material there is no peak to miss there; on
//                    a synthetic tone in the transition band the interpolated phases read low and the
//                    sample-peak floor is what remains.  That is why the field carries a scope and not
//                    just a number.
//     it assumes  -- a sinusoid, like its siblings; on a broadband transient the phases can add error
//                    of either sign (the gain bound's SQAM 64 datum is the measured case).
//     it applies to -- the INTERIOR reading, for the same reason as the gain bound.
//
// ⛔ THE PASSBAND EDGE IS A CONSTANT BESIDE THE TABLE AND DESCRIBES IT.  The table was designed with a
//   passband edge at 0.45 fs; the constant is not derived from the coefficients and the two could
//   drift.  The unit test guards that: the loss just inside the edge is small and the loss at fs/2 is
//   large, so a constant that named a different edge would fail.
inline constexpr double kTruePeakPassbandEdge = 0.45;   // of fs; the design's passband edge

// ⛔ COMPUTED FROM THE TABLE AT FIRST CALL, NEVER A LITERAL -- and the computation is a function of
//   (table, edge) so that the unit test can PLANT a table and assert the number MOVES.  A stale literal
//   fails that test; a function that ignored its table would too.  The scan is 8193 points on [0, edge]
//   plus a golden-section refinement; the cost is paid once per process.
inline double truePeakFilterLossBoundDbOf(const std::vector<std::vector<double>>& ph, double passbandEdge) {
    const int taps = (int)ph[0].size(), half = taps / 2;
    auto lossAt = [&](double f) {          // the WORST phase's attenuation at f, in dB (positive = loss)
        const double w = 2.0 * M_PI * f; double gmin = 1e300;
        for (const auto& hp : ph) {
            double re = 0.0, im = 0.0;
            for (int t = 0; t < taps; ++t) { const double m = (double)(t - half + 1); re += hp[t] * std::cos(w * m); im += hp[t] * std::sin(w * m); }
            const double a = std::sqrt(re * re + im * im); if (a < gmin) gmin = a;
        }
        return -20.0 * std::log10(gmin);
    };
    const int N = 8192; double best = -1e300, fb = 0.0;
    for (int k = 0; k <= N; ++k) { const double f = passbandEdge * (double)k / N; const double l = lossAt(f); if (l > best) { best = l; fb = f; } }
    double lo = fb - passbandEdge / N, hi = fb + passbandEdge / N; if (lo < 0.0) lo = 0.0; if (hi > passbandEdge) hi = passbandEdge;
    for (int it = 0; it < 80; ++it) {
        const double a = hi - 0.618033988749895 * (hi - lo), b = lo + 0.618033988749895 * (hi - lo);
        if (lossAt(a) < lossAt(b)) lo = a; else hi = b;
    }
    const double refined = lossAt(0.5 * (lo + hi));
    return refined > best ? refined : best;   // the refinement can only tighten; never report less than the scan saw
}
inline double truePeakFilterLossBoundDb() {
    static const double bound = truePeakFilterLossBoundDbOf(truePeakPolyphase(), kTruePeakPassbandEdge);
    return bound;
}

// THE FOURTH NUMBER: the estimator's worst under-read of a STEADY sine, the grid, the filter's magnitude
//   AND its phase error all in -- the tight low side.
//
// WHY THIS EXISTS.  The three numbers above are honest and LOOSE together: truePeakGridBoundDb is the
//   grid's worst case at fs/2, truePeakFilterLossBoundDb the filter's worst attenuation at the passband
//   edge, and their sum (0.256 dB for this table) is roughly twice what this estimator can actually do
//   to a sine (0.127 dB, at fs/4) -- because no one peak meets the two worst cases at once, and because
//   neither field carries the filter's PHASE error, which moves each phase's effective sampling
//   instant (measured: 0.0008 dB at fs/4 for this table).  A professional bracketing a reading deserves
//   the tight number, and a tight number that is a literal goes stale the day the table changes.
//
// WHAT IT IS.  A steady sine at f = a fs / b (lowest terms) has |x| repeating every b/(2a) samples, so its
//   extrema visit a fixed set of grid offsets -- ONE when b is even and a = 1 (fs/4, fs/6, ...: the tone never
//   gets a second look), otherwise a (b even) or 2a (b odd) offsets equally spaced -- and the tone reads the
//   BEST of them.  One extremum at position p is read by every instant as |H_g(f)| * |cos(2 pi f (t_g - p))|,
//   over the raw sample (H = 1 at t = 0) and every phase at its EFFECTIVE instant t_q = arg H_q(f) / (2 pi f)
//   (the design intends (2q+1)/16; the filter's phase error is the difference); the tone's reading is the
//   largest over its offsets, and W(a, b) is -20 log10 of the smallest that reading can be over the alignment.
//   The field is the maximum of W over every a/b with b <= 32 inside the passband (exhaustive), plus the
//   fs/(2m) tail.  For the table that ships: 0.126620 dB at fs/4 (one offset; the peak midway between phases
//   3 and 4, effective instants 0.436884 and 0.563116 of a sample), which is, to six places, what the
//   standard's own fs/4 tones (Tech 3341 cases 16 and 19) were measured low against an exact reference (doc
//   278) and what the meter reads on them.  ⛔ THE RUNNERS-UP ARE NOT IN THE fs/(2m) FAMILY: fs/3 (two
//   offsets) reads 0.1169 and 2fs/5 (four) 0.1133 -- their offsets are spaced at multiples of the grid's own
//   midpoint spacing, so every one of them can sit in a gap at once; a first draft of this field ran the
//   family alone and would have been 0.01 dB from being wrong.  THE MODEL IS THE METER: the test drives long
//   tones at those four frequencies and the meter reproduces W to better than 1e-6 dB at each.
//
// ⛔ WHAT IT COVERS AND WHAT IT DOES NOT:
//     it covers   -- a STEADY sine at any frequency inside the passband, any alignment, through the
//                    INTERIOR reading: the reading is never lower than the truth minus this number.  It
//                    is a LOW-side quantity, positive, and it is at most the sum of the two fields above
//                    (asserted) -- it does not replace them, each of which names a mechanism.
//     it EXCLUDES -- a LONE extremum with no neighbour of its own height (a transient), which the grid
//                    can miss at the EDGE frequency with no later extremum to rescue it: that worst case
//                    is larger (about 0.23 dB for this table, the single-extremum worst at the edge) and
//                    is bounded by the two fields' SUM, not by this one -- stated here so the tight number
//                    is not read as the transient number.  It excludes content above the passband edge,
//                    like its siblings.
//     it assumes  -- a sinusoid, AND HERE THE ASSUMPTION IS LOAD-BEARING.  The effective-instant model
//                    of each phase is exact for one and was measured against the meter to six places,
//                    but a bound derived from sines is a bound for sines.  The interpolation error's
//                    SIGN CHANGES across the band, so a signal band-limited to the passband can carry
//                    energy in phase where the filter loses and out of phase where it gains, and the
//                    ripple then adds coherently instead of cancelling.  Such a signal has been
//                    constructed and driven through this meter: it reads about 0.40 dB LOW -- past this
//                    number, past the single-extremum worst above, and past the two fields' sum.  It was
//                    found by SEARCH, not computed from the table, and the worst over all band-limited
//                    signals is not known to be smaller; programme material measured so far stays within
//                    0.14 dB.  The GRID half of the guarantee does extend to every band-limited signal
//                    (see truePeakGridBoundDb); the FILTER half does not, and no sine-derived number
//                    can make it.
//     it applies to -- the INTERIOR reading, for the same reason as its siblings.
//
// ⛔ COMPUTED FROM THE TABLE AT FIRST CALL, NEVER A LITERAL -- as a function of (table, edge) so the unit
//   test can PLANT a table (x0.99 moves it by exactly 20 log10(1/0.99)), plant an INSTANT error (phase 3 a
//   copy of phase 2 doubles a gap and the number rises), and name fs/2 as the edge (which admits m = 1 and
//   reads hundreds of dB: the edge is real).  The position scan is EVEN (128 points per offset spacing) so
//   the midpoint between two phases is ON the grid (a lesson learned), refined by golden section; b runs to 32
//   and the tail to fs/256, below which the grid term is under 0.0002 dB.  The cost is paid once per process.
inline double truePeakSineWorstDbOf(const std::vector<std::vector<double>>& ph, int a, int b) {
    // the worst under-read of a STEADY sine at f = a fs / b (a/b in lowest terms), in dB (positive = reads low).
    // |x| has period b/(2a) samples, so the tone's extrema visit  a  distinct grid offsets when b is even and  2a
    // when b is odd, equally spaced; the tone reads the BEST of them, and the worst tone is the alignment p0 at
    // which that best is smallest.  With a = 1 and b even (the fs/(2m) family) there is one offset: no second look.
    const int taps = (int)ph[0].size(), half = taps / 2, P = (int)ph.size();
    const double f = (double)a / (double)b, w = 2.0 * M_PI * f;
    std::vector<double> mag(P + 1, 1.0), tau(P + 1, 0.0);          // [0] is the raw sample: H = 1, instant 0
    for (int q = 0; q < P; ++q) {
        double re = 0.0, im = 0.0;
        for (int t = 0; t < taps; ++t) { const double mm = (double)(t - half + 1); re += ph[q][t] * std::cos(w * mm); im += ph[q][t] * std::sin(w * mm); }
        mag[q + 1] = std::sqrt(re * re + im * im); tau[q + 1] = std::atan2(im, re) / w;   // the EFFECTIVE instant
    }
    const double quarter = 0.25 / f; const int nmax = (int)std::ceil(quarter) + 1;
    auto reading = [&](double p) {                                 // the largest |value| any instant within a quarter period sees of ONE extremum at p
        double best = 0.0;                                         // (the tone's OTHER extrema are the offsets below; an instant a quarter period away reads 0)
        for (int n = -nmax; n <= nmax; ++n) for (int g = 0; g <= P; ++g) {
            const double d = (double)n + tau[g] - p; if (d > quarter || d < -quarter) continue;
            const double v = mag[g] * std::fabs(std::cos(w * d)); if (v > best) best = v;
        }
        return best;
    };
    const int nOff = (b % 2 == 0) ? a : 2 * a;                     // the offsets a steady tone visits, spaced 1/nOff
    const int N = 128 * nOff;                                      // EVEN, 128 points per offset spacing: the midpoint between two phases is on the grid
    std::vector<double> R((size_t)N);                              // one extremum's reading on the grid, computed ONCE per frequency
    for (int k = 0; k < N; ++k) R[(size_t)k] = reading((double)k / N);
    double worst = 1e300, pw = 0.0;
    for (int i = 0; i < N; ++i) {                                  // the tone reads the BEST of its offsets, 128 grid points apart
        double best = 0.0;
        for (int k = 0; k < nOff; ++k) { const double v = R[(size_t)((i + 128 * k) % N)]; if (v > best) best = v; }
        if (best < worst) { worst = best; pw = (double)i / N; }
    }
    auto toneReading = [&](double p0) {                            // the same, off the grid, for the refinement
        double best = 0.0;
        for (int k = 0; k < nOff; ++k) { const double v = reading(p0 + (double)k / nOff); if (v > best) best = v; }
        return best;
    };
    double lo = pw - 1.0 / N, hi = pw + 1.0 / N;
    for (int it = 0; it < 60; ++it) {
        const double x = hi - 0.618033988749895 * (hi - lo), y = lo + 0.618033988749895 * (hi - lo);
        if (toneReading(x) > toneReading(y)) lo = x; else hi = y;   // minimising the reading
    }
    const double refined = toneReading(0.5 * (lo + hi));
    const double r = refined < worst ? refined : worst;            // the refinement can only lower it; never report more than the scan saw
    return -20.0 * std::log10(r > 1e-300 ? r : 1e-300);
}
struct TruePeakSineLowBound { double db; double frequency; };    // frequency in units of fs
inline TruePeakSineLowBound truePeakSineLowBoundOf(const std::vector<std::vector<double>>& ph, double passbandEdge) {
    // EXHAUSTIVE over every frequency a fs / b in lowest terms with b <= 32 inside the passband (every tone whose
    // extrema visit few grid offsets lives there), plus the fs/(2m) tail to m = 128 for the low end, where the grid
    // term is under 0.0002 dB.  Larger denominators mean more offsets and a better-rescued tone; the reference
    // model (d284-sinelow.py) enumerates b <= 64 and finds nothing above the b <= 32 maximum for this table.
    TruePeakSineLowBound out{ -1e300, 0.0 };
    auto consider = [&](int a, int b) {
        const double f = (double)a / (double)b; if (f > passbandEdge) return;
        const double d = truePeakSineWorstDbOf(ph, a, b); if (d > out.db) { out.db = d; out.frequency = f; }
    };
    for (int b = 2; b <= 32; ++b) for (int a = 1; a < b; ++a) {
        int x = a, y = b; while (y) { const int t = x % y; x = y; y = t; }   // gcd
        if (x == 1) consider(a, b);
    }
    for (int m : { 48, 64, 96, 128 }) consider(1, 2 * m);
    return out;
}
inline double truePeakSineLowBoundDb() {
    static const TruePeakSineLowBound b = truePeakSineLowBoundOf(truePeakPolyphase(), kTruePeakPassbandEdge);
    return b.db;
}
inline double truePeakSineLowBoundFrequency() {                    // where the bound is attained, in units of fs (fs/4 for this table)
    static const TruePeakSineLowBound b = truePeakSineLowBoundOf(truePeakPolyphase(), kTruePeakPassbandEdge);
    return b.frequency;
}

// The maximum of |x| AND of all OS interpolated phases, taken over OUTPUT POSITIONS [lo, hi).
// ⛔ THE TAPS STILL REACH OUTSIDE [lo, hi) AND OUTSIDE THE BUFFER.  Restricting the range does not
//    change the filter; it declines to REPORT the filter's output where the filter was fed zeros
//    that are not in the signal.  That is the whole content of an "edge guard".
inline double truePeakLinearOver(const AudioBuffer& buf, int ch, size_t lo, size_t hi) {
    static const std::vector<std::vector<double>> ph = truePeakPolyphase();
    const int OS = kTruePeakOversampling, taps = (int)ph[0].size(), half = taps / 2;
    // The guard constant is derived from the tap count; if the table ever changes shape, the guard
    // is silently wrong.  This is the only place both are visible, so this is where it is checked.
    static_assert(kTruePeakEdgeGuard == kTruePeakTaps / 2, "guard must stay taps/2");
    assert(taps == kTruePeakTaps);
    // ⛔ AND THE SAME TIE FOR THE OVERSAMPLING FACTOR.  This loop used a LOCAL literal 4 while the
    //   disclosed bound is computed from kTruePeakOversampling: two numbers describing one thing,
    //   free to drift, with the payload telling a user about the one the loop does not use.  That
    //   is a drift this project closes here rather than leaving to be found.
    assert((int)ph.size() == kTruePeakOversampling);
    double peak = 0.0;
    for (size_t i = lo; i < hi; ++i) {
        double s0 = std::fabs((double)buf.at(i, ch));
        if (s0 > peak) peak = s0;
        for (int p = 0; p < OS; ++p) {
            double acc = 0.0;
            for (int t = 0; t < taps; ++t) {
                long idx = (long)i + (t - half + 1);
                if (idx < 0 || idx >= (long)buf.frames) continue;
                acc += ph[p][t] * (double)buf.at((size_t)idx, ch);
            }
            double a = std::fabs(acc);
            if (a > peak) peak = a;
        }
    }
    return peak;
}

// The maximum of |x| over the input samples AND over all OS interpolated phases. The input sample
// is kept as a floor on purpose: the table alone can read a sample-aligned peak BELOW the sample (its
// nearest evaluation point is 1/16 sample away and a phase can sit at -0.09 dB), and a true peak
// reported below the sample peak is not a reading anyone should act on.
// ⛔ UNCHANGED.  It is the whole-buffer maximum, it stays the whole-buffer maximum, and
//    the number published in 1.17.0 does not move.  That is exactly what is forbidden here.
inline double truePeakDb(const AudioBuffer& buf, int ch) {
    return linToDb(truePeakLinearOver(buf, ch, 0, buf.frames));
}

// THE SAME MAXIMUM WITH kTruePeakEdgeGuard SAMPLES EXCLUDED AT BOTH ENDS.
//
// WHY IT EXISTS.  The polyphase interpolator is fed ZEROS outside the buffer, and a buffer
//   whose first or last sample is non-zero therefore presents the filter with a step it must ring
//   at.  That ring is REAL for a rendered file -- silence genuinely precedes its first sample at
//   every listener's DAC -- so truePeakDb must keep reporting it.  But `analysis.meter` renders
//   startPos->endPos, so EVERY meter call has two raw edges wherever the user put them, and on
//   ordinary material the reported true peak moves by up to 0.94 dB with the window (measured
//   arm B, measured on the running server).  Two numbers that disagree say "the peak is at an
//   edge"; one number cannot.
//
// ⛔ A GUARDED POSITION IS SKIPPED ENTIRELY -- its interpolated phases AND its raw sample.  The
//   alternative (keep the sample floor over the whole buffer, guard only the phases) is defensible
//   and was REJECTED for one reason: THIS one is what was measured, and shipping the other would make
//   its "2 samples suffices" a claim about a different quantity.
// ⚠️ THE PRICE, STATED RATHER THAN LEFT TO BE FOUND: this number is NOT bounded below by the sample
//   peak.  A lone full-scale sample at index 0 reads samplePeak +0.000000 and an interior tens of
//   dB below it.  The invariant truePeakDb carries does NOT transfer, and the unit suite asserts
//   the gap ON PURPOSE so nobody "fixes" the field into breaking the definition.
// ⛔ RETURNS false, AND WRITES NOTHING, when the buffer has no interior at all (frames <= 2*guard).
//   A floor value there would look like a reading, and an absence is stated rather than floored.
inline bool truePeakDbInterior(const AudioBuffer& buf, int ch, double& outDb,
                               int guard = kTruePeakEdgeGuard) {
    if (guard < 0) return false;
    const size_t g = (size_t)guard;
    if (buf.frames <= 2 * g) return false;
    outDb = linToDb(truePeakLinearOver(buf, ch, g, buf.frames - g));
    return true;
}

// ONE PASS, TWO MAXIMA.  analyzeChannel() needs both the whole-buffer maximum (truePeakDb) and the
// edge-guarded one (truePeakDbInterior), and until 1.20.0 it computed them with two full passes of
// the interpolator over the same samples.  This folds every output position's value -- the raw sample
// and all OS interpolated phases -- into the whole-buffer maximum always, and into the interior maximum
// when the position lies inside [guard, frames - guard).  The per-position values are the SAME doubles
// truePeakLinearOver computes, and a maximum is order-independent, so both results are bit-identical
// to the two-pass ones; the unit suite asserts that over the same fixtures the two-pass functions are
// measured on.  truePeakDb() and truePeakDbInterior() are unchanged for their other callers.
// ⛔ THE INTERIOR HALF KEEPS truePeakDbInterior's CONTRACT: no interior when frames <= 2*guard, and a
//    guarded position is skipped ENTIRELY for the interior maximum, sample and phases alike.
inline void truePeakLinearFused(const AudioBuffer& buf, int ch, int guard,
                                double& outFull, double& outInterior, bool& interiorValid) {
    static const std::vector<std::vector<double>> ph = truePeakPolyphase();
    const int OS = kTruePeakOversampling, taps = (int)ph[0].size(), half = taps / 2;
    static_assert(kTruePeakEdgeGuard == kTruePeakTaps / 2, "guard must stay taps/2");
    assert(taps == kTruePeakTaps);
    assert((int)ph.size() == kTruePeakOversampling);
    const size_t g = guard < 0 ? (size_t)0 : (size_t)guard;
    interiorValid = (guard >= 0) && (buf.frames > 2 * g);
    const size_t ilo = interiorValid ? g : 0, ihi = interiorValid ? buf.frames - g : 0;
    double full = 0.0, interior = 0.0;
    for (size_t i = 0; i < buf.frames; ++i) {
        double v = std::fabs((double)buf.at(i, ch));
        for (int p = 0; p < OS; ++p) {
            double acc = 0.0;
            for (int t = 0; t < taps; ++t) {
                long idx = (long)i + (t - half + 1);
                if (idx < 0 || idx >= (long)buf.frames) continue;
                acc += ph[p][t] * (double)buf.at((size_t)idx, ch);
            }
            double a = std::fabs(acc);
            if (a > v) v = a;
        }
        if (v > full) full = v;
        if (interiorValid && i >= ilo && i < ihi && v > interior) interior = v;
    }
    outFull = full;
    outInterior = interior;
}

// ------------------------------------------------------------------------------------------------
// Per-channel metrics + inter-channel correlation
// ------------------------------------------------------------------------------------------------

struct ChannelMetrics {
    double rmsDb = kMinDb();
    double peakDb = kMinDb();
    double truePeakDb = kMinDb();
    double kLevelLkfs = kMinDb();  // ungated K-weighted level (per-channel loudness contribution)

    // ---- The edge-guard reading, beside the number it explains. ----
    // truePeakDbInterior is the SAME maximum with kTruePeakEdgeGuard samples excluded at BOTH ends.
    // interiorValid is false when the buffer is too short to HAVE an interior (frames <= 2*guard);
    //   the payload reports null there rather than a floor value that would look like a reading.
    // edgeDominated is true when the whole-buffer maximum is attained ONLY inside a guard region,
    //   i.e. truePeakDb > truePeakDbInterior.  ⚠️ A TIE READS false ON PURPOSE: if the same level
    //   also occurs in the interior, the reported peak is not an edge artefact and nothing is
    //   hidden by saying so.
    double truePeakDbInterior = kMinDb();
    bool   truePeakInteriorValid = false;
    bool   truePeakEdgeDominated = false;
};

inline ChannelMetrics analyzeChannel(const AudioBuffer& buf, int ch) {
    ChannelMetrics m;
    if (buf.frames == 0) return m;
    double sum = 0.0, peak = 0.0;
    for (size_t i = 0; i < buf.frames; ++i) {
        double v = buf.at(i, ch);
        sum += v * v;
        double a = std::fabs(v);
        if (a > peak) peak = a;
    }
    double ms = sum / (double)buf.frames;
    m.rmsDb = powToDb(ms);
    m.peakDb = linToDb(peak);
    // One pass of the interpolator for both true-peak numbers (truePeakLinearFused, above); the
    // two-pass form -- truePeakDb() then truePeakDbInterior() -- is what this is bit-identical to.
    double fullLin = 0.0, interiorLin = 0.0; bool interiorValid = false;
    truePeakLinearFused(buf, ch, kTruePeakEdgeGuard, fullLin, interiorLin, interiorValid);
    m.truePeakDb = linToDb(fullLin);
    m.truePeakInteriorValid = interiorValid;
    if (m.truePeakInteriorValid) {
        const double interior = linToDb(interiorLin);
        m.truePeakDbInterior = interior;
        m.truePeakEdgeDominated = (m.truePeakDb > interior);
    }
    double kms = kWeightedMeanSquare(buf, ch);
    m.kLevelLkfs = kms > 0.0 ? (-0.691 + 10.0 * std::log10(kms)) : kMinDb();
    return m;
}

// Pearson correlation of two channels over the whole buffer. +1 identical, 0 uncorrelated, -1 inverted.
// This is the classic phase/mono-compatibility meter when applied to an L/R pair.
inline double interChannelCorrelation(const AudioBuffer& buf, int a, int b) {
    if (buf.frames == 0) return 0.0;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (size_t i = 0; i < buf.frames; ++i) {
        double x = buf.at(i, a), y = buf.at(i, b);
        sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }
    double n = (double)buf.frames;
    double cov = sab - sa * sb / n;
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    double den = std::sqrt(va * vb);
    if (den < 1e-20) return 0.0;  // a silent channel has no defined correlation
    return clampd(cov / den, -1.0, 1.0);
}

// ------------------------------------------------------------------------------------------------
// Direct-sample helpers (feed by the render-free audio-accessor path — audio_accessor.h)
//
// These operate on the SAME AudioBuffer the render path produces, so a buffer read straight off a
// track/take accessor is analyzed by exactly the same DSP as a rendered stem. All SDK-free and
// unit-tested with synthetic buffers (unit.meter).
// ------------------------------------------------------------------------------------------------

// Per-channel DC offset (mean sample value). A non-trivial value flags a DC-biased capture.
inline double channelDcOffset(const AudioBuffer& buf, int ch) {
    if (buf.frames == 0) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < buf.frames; ++i) s += buf.at(i, ch);
    return s / (double)buf.frames;
}

// Count samples at/above a full-scale clip threshold (|x| >= thresh) on one channel.
inline size_t channelClipCount(const AudioBuffer& buf, int ch, double thresh = 0.999) {
    size_t n = 0;
    for (size_t i = 0; i < buf.frames; ++i)
        if (std::fabs((double)buf.at(i, ch)) >= thresh) ++n;
    return n;
}

// Decimated min/max waveform overview: split channel `ch` into `buckets` equal frame ranges and record
// the [min,max] sample in each — a compact, faithful envelope for a waveform preview. Empty if silent
// input or buckets<1; buckets is capped to the frame count so every bucket spans >=1 frame.
struct OverviewBucket { float min = 0.0f; float max = 0.0f; };
inline std::vector<OverviewBucket> channelOverview(const AudioBuffer& buf, int ch, int buckets) {
    std::vector<OverviewBucket> out;
    if (buf.frames == 0 || buckets < 1) return out;
    if ((size_t)buckets > buf.frames) buckets = (int)buf.frames;
    out.reserve((size_t)buckets);
    for (int b = 0; b < buckets; ++b) {
        size_t lo = (size_t)((double)b * (double)buf.frames / (double)buckets);
        size_t hi = (size_t)((double)(b + 1) * (double)buf.frames / (double)buckets);
        if (hi <= lo) hi = lo + 1;
        float mn = 1.0e30f, mx = -1.0e30f;
        for (size_t i = lo; i < hi && i < buf.frames; ++i) {
            const float v = buf.at(i, ch);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        out.push_back(OverviewBucket{mn, mx});
    }
    return out;
}

// A contiguous silent region [startFrame, endFrame) where the trailing-window cross-channel RMS stays
// below threshDb (an RMS level in dBFS). Frame units; the tool layer maps them to seconds.
struct SilentRegion { size_t startFrame = 0; size_t endFrame = 0; };

// Scan for silent regions: per-frame cross-channel mean power -> trailing-window RMS -> classify vs a
// power threshold (10^(threshDb/10)); coalesce runs and drop any shorter than minFrames. windowFrames
// smooths the detector (e.g. 10 ms) so an isolated spike in a noise floor doesn't split a silence.
inline std::vector<SilentRegion> scanSilence(const AudioBuffer& buf, double threshDb,
                                             size_t windowFrames, size_t minFrames) {
    std::vector<SilentRegion> out;
    if (buf.frames == 0 || buf.channels < 1) return out;
    if (windowFrames < 1) windowFrames = 1;
    std::vector<double> pf(buf.frames);
    for (size_t i = 0; i < buf.frames; ++i) {
        double s = 0.0;
        for (int c = 0; c < buf.channels; ++c) { const double v = buf.at(i, c); s += v * v; }
        pf[i] = s / (double)buf.channels;
    }
    std::vector<double> P(buf.frames + 1, 0.0);
    for (size_t i = 0; i < buf.frames; ++i) P[i + 1] = P[i] + pf[i];
    const double thrLin = std::pow(10.0, threshDb / 10.0);  // power threshold (threshDb = RMS level, dBFS)
    bool inRun = false; size_t runStart = 0;
    auto closeRun = [&](size_t end) {
        if (inRun && end - runStart >= minFrames) out.push_back(SilentRegion{runStart, end});
        inRun = false;
    };
    for (size_t i = 0; i < buf.frames; ++i) {
        const size_t lo = (i + 1 >= windowFrames) ? (i + 1 - windowFrames) : 0;
        const double meanPow = (P[i + 1] - P[lo]) / (double)(i + 1 - lo);
        const bool silent = meanPow < thrLin;
        if (silent && !inRun) { inRun = true; runStart = i; }
        else if (!silent && inRun) closeRun(i);
    }
    closeRun(buf.frames);
    return out;
}

// ------------------------------------------------------------------------------------------------
// BS.1770-4 GATED program loudness + momentary/short-term timeline.
//
// The C++ loudness path for signals REAPER's RENDER_STATS cannot measure directly: stems read back
// from a bit-exact render, dialog buses, and downmixes that exist only in memory. Program loudness of
// a rendered master stays REAPER-native; these functions carry the per-stem / dialog /
// timeline metrics. Channel weighting follows BS.1770-4 Tables 4/5 exactly (F33): G = 1.41
// ONLY for ear-level channels with |az| 60°–120° inclusive (Ls/Rs ±110, Lss/Rss ±90, Lw/Rw ±60);
// rear surrounds (Lsr/Rsr ±135) and all heights 1.0; LFE excluded (0). The weight multiplies POWER
// (w·z² below, so 1.41 ≈ +1.5 dB). The tool layer derives the vector from the SMPTE bed labels via
// bed_weights.h and passes it in (empty weights = all 1.0).
// ------------------------------------------------------------------------------------------------

// Per-sample K-weighted, channel-weighted power series p[i] = sum_ch G_ch * z_ch[i]^2 (z = K-filtered).
// One channel is filtered at a time, so peak memory is ~2 doubles per frame regardless of width.
inline std::vector<double> kWeightedPowerSeries(const AudioBuffer& buf,
                                                const std::vector<double>& weights = {}) {
    std::vector<double> p(buf.frames, 0.0);
    if (buf.frames == 0 || buf.sampleRate <= 0.0) return p;
    const Biquad s1 = k1ShelfBiquad(buf.sampleRate);
    const Biquad s2 = k2HighpassBiquad(buf.sampleRate);
    std::vector<double> x(buf.frames);
    for (int ch = 0; ch < buf.channels; ++ch) {
        const double w = (ch < (int)weights.size()) ? weights[ch] : 1.0;
        if (w <= 0.0) continue;  // LFE (and any explicitly zero-weighted channel) is excluded
        for (size_t i = 0; i < buf.frames; ++i) x[i] = buf.at(i, ch);
        s1.process(x);
        s2.process(x);
        for (size_t i = 0; i < buf.frames; ++i) p[i] += w * x[i] * x[i];
    }
    return p;
}

inline double lufsFromPower(double pw) { return pw > 1e-24 ? -0.691 + 10.0 * std::log10(pw) : kMinDb(); }

struct GatedLoudness {
    bool valid = false;
    double integratedLufs = kMinDb();  // BS.1770-4 two-stage gated (-70 LKFS absolute, -10 LU relative)
    double ungatedLufs = kMinDb();     // whole-buffer K-weighted loudness, no gate
    int totalBlocks = 0;               // 400 ms blocks at 75% overlap
    int gatedBlocks = 0;               // blocks surviving both gates
    double activityFraction = 0.0;     // gatedBlocks / totalBlocks (the "how much of it is loud" readout)
};

// BS.1770-4 gated integrated loudness: 400 ms blocks, 75% overlap (100 ms hop); absolute gate at
// -70 LKFS; relative gate 10 LU below the mean power of the absolutely-gated blocks. On an ISOLATED
// stem (e.g. a dialog bus) the gates drop the silence between phrases, so the gated value approximates
// the level while the stem is active — activityFraction says how much of the program that was.
inline GatedLoudness gatedLoudness(const AudioBuffer& buf, const std::vector<double>& weights = {}) {
    GatedLoudness g;
    if (buf.frames == 0 || buf.sampleRate <= 0.0) return g;
    const std::vector<double> p = kWeightedPowerSeries(buf, weights);
    std::vector<double> P(buf.frames + 1, 0.0);
    for (size_t i = 0; i < buf.frames; ++i) P[i + 1] = P[i] + p[i];
    g.ungatedLufs = lufsFromPower(P[buf.frames] / (double)buf.frames);

    const size_t blockLen = (size_t)std::llround(0.400 * buf.sampleRate);
    const size_t hop = (size_t)std::llround(0.100 * buf.sampleRate);
    if (blockLen == 0 || hop == 0) return g;
    std::vector<double> bp;  // block mean powers
    for (size_t s = 0; s + blockLen <= buf.frames; s += hop)
        bp.push_back((P[s + blockLen] - P[s]) / (double)blockLen);
    g.valid = true;
    if (bp.empty()) {  // shorter than one 400 ms block: no gating possible, report ungated
        g.integratedLufs = g.ungatedLufs;
        return g;
    }
    g.totalBlocks = (int)bp.size();
    double sum1 = 0.0; int n1 = 0;
    for (double pw : bp) if (lufsFromPower(pw) > -70.0) { sum1 += pw; ++n1; }
    if (n1 == 0) { g.integratedLufs = kMinDb(); return g; }         // everything below the absolute gate
    const double relThresh = lufsFromPower(sum1 / n1) - 10.0;
    double sum2 = 0.0; int n2 = 0;
    for (double pw : bp) {
        const double l = lufsFromPower(pw);
        if (l > -70.0 && l > relThresh) { sum2 += pw; ++n2; }
    }
    if (n2 == 0) { g.integratedLufs = kMinDb(); return g; }
    g.gatedBlocks = n2;
    g.activityFraction = (double)n2 / (double)g.totalBlocks;
    g.integratedLufs = lufsFromPower(sum2 / n2);
    return g;
}

struct LoudnessTimeline {
    bool valid = false;
    double hopSec = 0.1;
    std::vector<double> times;       // window END time in seconds from the buffer start
    std::vector<double> momentary;   // 400 ms window (first value once a full block exists)
    std::vector<double> shortTerm;   // 3 s window; grows from the start until 3 s is available
    double maxMomentaryLufs = kMinDb(); double maxMomentaryTime = 0.0;
    double maxShortTermLufs = kMinDb(); double maxShortTermTime = 0.0;
    // EBU Tech 3342 loudness range from the full-3s short-term distribution (-70 absolute gate,
    // -20 LU relative gate, 10th..95th percentile).
    double lraLowLufs = kMinDb(); double lraHighLufs = kMinDb(); double lraLu = 0.0;
};

inline LoudnessTimeline loudnessTimeline(const AudioBuffer& buf,
                                         const std::vector<double>& weights = {},
                                         double hopSec = 0.1) {
    LoudnessTimeline tl;
    if (buf.frames == 0 || buf.sampleRate <= 0.0 || hopSec <= 0.0) return tl;
    tl.hopSec = hopSec;
    const std::vector<double> p = kWeightedPowerSeries(buf, weights);
    std::vector<double> P(buf.frames + 1, 0.0);
    for (size_t i = 0; i < buf.frames; ++i) P[i + 1] = P[i] + p[i];

    const size_t hop = std::max<size_t>(1, (size_t)std::llround(hopSec * buf.sampleRate));
    const size_t mLen = (size_t)std::llround(0.400 * buf.sampleRate);
    const size_t sLen = (size_t)std::llround(3.0 * buf.sampleRate);
    if (mLen == 0 || buf.frames < mLen) return tl;
    tl.valid = true;

    std::vector<double> stFull;  // full-3s short-term values (the LRA population)
    for (size_t end = mLen; end <= buf.frames; end += hop) {
        const double t = (double)end / buf.sampleRate;
        const double mPow = (P[end] - P[end - mLen]) / (double)mLen;
        const size_t sWin = std::min(end, sLen);
        const double sPow = (P[end] - P[end - sWin]) / (double)sWin;
        const double mL = lufsFromPower(mPow), sL = lufsFromPower(sPow);
        tl.times.push_back(t);
        tl.momentary.push_back(mL);
        tl.shortTerm.push_back(sL);
        if (mL > tl.maxMomentaryLufs) { tl.maxMomentaryLufs = mL; tl.maxMomentaryTime = t; }
        if (sL > tl.maxShortTermLufs) { tl.maxShortTermLufs = sL; tl.maxShortTermTime = t; }
        if (end >= sLen) stFull.push_back(sL);
    }

    // LRA (EBU Tech 3342). Needs at least a handful of full short-term windows to mean anything.
    std::vector<double> pool = stFull.empty() ? tl.shortTerm : stFull;
    std::vector<double> gated;
    double sumP = 0.0; int n = 0;
    for (double l : pool) if (l > -70.0) { sumP += std::pow(10.0, (l + 0.691) / 10.0); ++n; }
    if (n > 0) {
        const double rel = lufsFromPower(sumP / n) - 20.0;
        for (double l : pool) if (l > -70.0 && l > rel) gated.push_back(l);
    }
    if (!gated.empty()) {
        std::sort(gated.begin(), gated.end());
        auto pct = [&gated](double q) {
            const double idx = q * (double)(gated.size() - 1);
            const size_t lo = (size_t)idx;
            const size_t hi = std::min(lo + 1, gated.size() - 1);
            const double fr = idx - (double)lo;
            return gated[lo] * (1.0 - fr) + gated[hi] * fr;
        };
        tl.lraLowLufs = pct(0.10);
        tl.lraHighLufs = pct(0.95);
        tl.lraLu = tl.lraHighLufs - tl.lraLowLufs;
    }
    return tl;
}

// ------------------------------------------------------------------------------------------------
// Downmix — apply an [outCh][inCh] coefficient matrix. The tool layer builds the matrix
// (ITU-R BS.775-style fold-down from the SMPTE bed labels); the DSP just applies it, so the fold
// exists as a real buffer that the loudness / true-peak / correlation meters above can measure.
// ------------------------------------------------------------------------------------------------

inline AudioBuffer applyDownmixMatrix(const AudioBuffer& in,
                                      const std::vector<std::vector<double>>& matrix) {
    AudioBuffer out;
    out.channels = (int)matrix.size();
    out.sampleRate = in.sampleRate;
    out.frames = in.frames;
    if (out.channels == 0) return out;
    out.samples.assign(out.frames * (size_t)out.channels, 0.0f);
    for (size_t i = 0; i < in.frames; ++i) {
        for (int oc = 0; oc < out.channels; ++oc) {
            const std::vector<double>& row = matrix[oc];
            const int nin = std::min((int)row.size(), in.channels);
            double acc = 0.0;
            for (int ic = 0; ic < nin; ++ic) acc += row[ic] * (double)in.at(i, ic);
            out.samples[i * (size_t)out.channels + oc] = (float)acc;
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Real spherical harmonics (ambiX: SN3D, ACN ordering, no Condon-Shortley phase)
// ------------------------------------------------------------------------------------------------

// Associated Legendre P_l^m(x) WITHOUT the Condon-Shortley phase (ambiX convention), m >= 0.
inline double legendreNoCS(int l, int m, double x) {
    // P_m^m. The positive-product seed (fact*somx2, fact = 1,3,5,...) is the ambiX/geodesy convention
    // WITHOUT the Condon-Shortley (-1)^m phase — exactly what SN3D real SH need. (The Numerical-Recipes
    // recurrence instead seeds with -fact*somx2 to carry the CS phase; we deliberately do not.)
    double pmm = 1.0;
    if (m > 0) {
        double somx2 = std::sqrt(std::max(0.0, 1.0 - x * x));
        double fact = 1.0;
        for (int i = 1; i <= m; ++i) { pmm *= fact * somx2; fact += 2.0; }
    }
    if (l == m) return pmm;
    double pmmp1 = x * (2.0 * m + 1.0) * pmm;
    if (l == m + 1) return pmmp1;
    double pll = 0.0;
    for (int ll = m + 2; ll <= l; ++ll) {
        pll = ((2.0 * ll - 1.0) * x * pmmp1 - (ll + m - 1.0) * pmm) / (double)(ll - m);
        pmm = pmmp1; pmmp1 = pll;
    }
    return pll;
}

// SN3D normalization factor N_l^|m| = sqrt((2 - delta_{m0}) * (l-|m|)! / (l+|m|)!).
inline double sn3dNorm(int l, int m) {
    int am = std::abs(m);
    double num = (am == 0) ? 1.0 : 2.0;
    double ratio = 1.0;  // (l-am)! / (l+am)!
    for (int k = l - am + 1; k <= l + am; ++k) ratio /= (double)k;
    return std::sqrt(num * ratio);
}

// Real SN3D SH for ACN channel k, evaluated at ambiX-frame azimuth/elevation (radians).
inline double realSHsn3d(int k, double azAmbi, double elAmbi) {
    int l = (int)std::floor(std::sqrt((double)k) + 1e-9);
    int m = k - l * l - l;
    int am = std::abs(m);
    double leg = legendreNoCS(l, am, std::sin(elAmbi));
    double norm = sn3dNorm(l, am);
    if (m > 0) return norm * leg * std::cos(am * azAmbi);
    if (m < 0) return norm * leg * std::sin(am * azAmbi);
    return norm * leg;
}

// ------------------------------------------------------------------------------------------------
// Direction helpers. Vectors are carried in a right-handed frame (x=right, y=front, z=up); the
// REPORTED azimuth is ambisonic-standard (CCW from front, +az = LEFT = -x), matching the encoders.
// ------------------------------------------------------------------------------------------------

struct Dir { double x, y, z; };  // internal right-handed axes (x=right, y=front, z=up)

inline void vecToAzEl(const Dir& d, double& azDeg, double& elDeg) {
    azDeg = rad2deg(std::atan2(-d.x, d.y));                         // 0=front, +90=left (CCW, ambiX/IEM)
    elDeg = rad2deg(std::atan2(d.z, std::hypot(d.x, d.y)));         // +90=overhead
}

// A near-uniform virtual-loudspeaker layout on the sphere (Fibonacci spiral) — the decode grid for rE.
inline std::vector<Dir> fibonacciSphere(int n) {
    std::vector<Dir> pts;
    pts.reserve(n);
    const double ga = M_PI * (3.0 - std::sqrt(5.0));  // golden angle
    for (int i = 0; i < n; ++i) {
        double z = 1.0 - 2.0 * (i + 0.5) / n;
        double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        double th = ga * i;
        pts.push_back(Dir{r * std::cos(th), r * std::sin(th), z});
    }
    return pts;
}

// ------------------------------------------------------------------------------------------------
// Ambisonic field analysis: DirAC intensity DoA + diffuseness, plus Gerzon rV / rE localization vectors
//
// DirAC active-intensity DoA/diffuseness after V. Pulkki, "Directional Audio Coding" (AES, 2007);
// Gerzon rV/rE energy/velocity vectors after M. Gerzon, "General Metatheory of Auditory Localisation"
// (AES, 1992).
// ------------------------------------------------------------------------------------------------

struct AmbiField {
    int order = 0;
    int acnUsed = 0;
    bool valid = false;
    // DirAC (first-order active intensity)
    double doaAzDeg = 0, doaElDeg = 0;
    double diffuseness = 0;   // 0 = single plane wave, 1 = fully diffuse
    double directivity = 0;   // 1 - diffuseness
    // Velocity vector rV (pressure-velocity; |rV|=1 for a plane wave)
    double rVmag = 0, rVazDeg = 0, rVelDeg = 0;
    // Energy vector rE (virtual-speaker decode; |rE|<1, sharper with higher order)
    double rEmag = 0, rEazDeg = 0, rEelDeg = 0;
    int rEspeakers = 0;
    std::vector<double> acnEnergyFrac;  // per-ACN energy as a fraction of total
};

inline int ambiOrderFromChannels(int nch) {
    if (nch < 4) return 0;
    int o = (int)std::floor(std::sqrt((double)nch) + 1e-9) - 1;
    return o < 1 ? 1 : o;
}

// Analyze a frame range [frameBegin, frameEnd) of an ambisonic (ACN) buffer — the building block for
// both the whole-buffer field and the windowed timeline. `order` = ambisonic order to use
// (<=0 => infer from channels). `isN3D` scales the first-order dipoles by 1/sqrt(3) so the SN3D
// intensity relations hold.
inline AmbiField analyzeAmbisonicRange(const AudioBuffer& buf, size_t frameBegin, size_t frameEnd,
                                       int order = 0, bool isN3D = false, int rESpeakerCount = 36) {
    AmbiField f;
    int inferred = ambiOrderFromChannels(buf.channels);
    f.order = (order > 0) ? order : inferred;
    if (f.order < 1) f.order = 1;
    f.acnUsed = std::min(buf.channels, (f.order + 1) * (f.order + 1));
    if (frameEnd > buf.frames) frameEnd = buf.frames;
    if (buf.channels < 4 || frameBegin >= frameEnd || f.acnUsed < 4) { f.valid = false; return f; }
    f.valid = true;

    const double dipScale = isN3D ? (1.0 / std::sqrt(3.0)) : 1.0;  // N3D dipoles are sqrt(3)x SN3D

    // ---- First-order intensity (DirAC) ----
    // SN3D mapping: ambiX axes X=front(ACN3), Y=left(ACN1), Z=up(ACN2). Project: x=right=-left.
    double sWW = 0, sIx = 0, sIy = 0, sIz = 0, sDip = 0;
    for (size_t i = frameBegin; i < frameEnd; ++i) {
        double W = buf.at(i, 0);
        double Yl = buf.at(i, 1) * dipScale;  // left
        double Zu = buf.at(i, 2) * dipScale;  // up
        double Xf = buf.at(i, 3) * dipScale;  // front
        sWW += W * W;
        sDip += Xf * Xf + Yl * Yl + Zu * Zu;
        sIx += W * (-Yl);  // project x (right)
        sIy += W * (Xf);   // project y (front)
        sIz += W * (Zu);   // project z (up)
    }
    double n = (double)(frameEnd - frameBegin);
    Dir I{sIx / n, sIy / n, sIz / n};
    double Imag = std::sqrt(I.x * I.x + I.y * I.y + I.z * I.z);
    double energy = 0.5 * (sWW / n + sDip / n);  // DirAC energy density (SN3D B-format)
    vecToAzEl(I, f.doaAzDeg, f.doaElDeg);
    f.diffuseness = clampd(energy > 1e-20 ? 1.0 - Imag / energy : 1.0, 0.0, 1.0);
    f.directivity = 1.0 - f.diffuseness;

    // ---- Velocity vector rV = <W*u> / <W^2>  (|rV|=1 for a plane wave, ->0 diffuse) ----
    double rVx = 0, rVy = 0, rVz = 0;
    if (sWW > 1e-20) { rVx = sIx / sWW; rVy = sIy / sWW; rVz = sIz / sWW; }
    Dir rV{rVx, rVy, rVz};
    f.rVmag = std::sqrt(rVx * rVx + rVy * rVy + rVz * rVz);
    vecToAzEl(rV, f.rVazDeg, f.rVelDeg);

    // ---- Energy vector rE via a projection decode onto a Fibonacci virtual-speaker layout ----
    // For each virtual speaker s at direction d_s, gain g_s(t) = sum_k a_k(t) * Y_k^SN3D(d_s). The
    // per-speaker energy E_s = <g_s^2>; rE = sum_s E_s d_s / sum_s E_s. Higher order => sharper rE.
    std::vector<Dir> spk = fibonacciSphere(rESpeakerCount);
    f.rEspeakers = (int)spk.size();
    // Precompute SH gains per speaker (in the ambiX frame).
    std::vector<std::vector<double>> shg(spk.size(), std::vector<double>(f.acnUsed, 0.0));
    for (size_t s = 0; s < spk.size(); ++s) {
        // project (x=right,y=front,z=up) -> ambiX (X=front,Y=left,Z=up); az CCW-from-front.
        double axf = spk[s].y, ayl = -spk[s].x, azu = spk[s].z;
        double azAmbi = std::atan2(ayl, axf);
        double elAmbi = std::atan2(azu, std::hypot(axf, ayl));
        for (int k = 0; k < f.acnUsed; ++k) shg[s][k] = realSHsn3d(k, azAmbi, elAmbi);
    }
    std::vector<double> espk(spk.size(), 0.0);
    for (size_t i = frameBegin; i < frameEnd; ++i) {
        for (size_t s = 0; s < spk.size(); ++s) {
            double g = 0.0;
            for (int k = 0; k < f.acnUsed; ++k) g += buf.at(i, k) * shg[s][k];
            espk[s] += g * g;
        }
    }
    double eTot = 0.0; Dir rE{0, 0, 0};
    for (size_t s = 0; s < spk.size(); ++s) {
        eTot += espk[s];
        rE.x += espk[s] * spk[s].x; rE.y += espk[s] * spk[s].y; rE.z += espk[s] * spk[s].z;
    }
    if (eTot > 1e-20) { rE.x /= eTot; rE.y /= eTot; rE.z /= eTot; }
    f.rEmag = std::sqrt(rE.x * rE.x + rE.y * rE.y + rE.z * rE.z);
    vecToAzEl(rE, f.rEazDeg, f.rEelDeg);

    // ---- Per-ACN energy distribution (fraction of total) ----
    f.acnEnergyFrac.assign(f.acnUsed, 0.0);
    double acnTot = 0.0;
    for (int k = 0; k < f.acnUsed; ++k) {
        double e = 0.0;
        for (size_t i = frameBegin; i < frameEnd; ++i) { double v = buf.at(i, k); e += v * v; }
        f.acnEnergyFrac[k] = e; acnTot += e;
    }
    if (acnTot > 1e-20) for (double& v : f.acnEnergyFrac) v /= acnTot;

    return f;
}

// Whole-buffer field (analyze the entire buffer in one range).
inline AmbiField analyzeAmbisonic(const AudioBuffer& buf, int order = 0, bool isN3D = false,
                                  int rESpeakerCount = 36) {
    if (buf.frames == 0) { AmbiField f; f.order = std::max(1, (order > 0) ? order : ambiOrderFromChannels(buf.channels)); f.acnUsed = std::min(buf.channels, (f.order + 1) * (f.order + 1)); return f; }
    return analyzeAmbisonicRange(buf, 0, buf.frames, order, isN3D, rESpeakerCount);
}

// ------------------------------------------------------------------------------------------------
// Windowed spatial-field timeline — the trajectory of a moving source: per-window DirAC
// DoA + diffuseness + rV/rE. Windows start at t=0 and step by hopSec; only full windows are emitted
// (a buffer shorter than one window yields a single window over everything).
// ------------------------------------------------------------------------------------------------

struct AmbiFieldWindow {
    double timeSec = 0.0;   // window START in seconds from the buffer start
    AmbiField field;
};

inline std::vector<AmbiFieldWindow> analyzeAmbisonicTimeline(const AudioBuffer& buf,
                                                             double windowSec, double hopSec,
                                                             int order = 0, bool isN3D = false,
                                                             int rESpeakerCount = 36) {
    std::vector<AmbiFieldWindow> out;
    if (buf.frames == 0 || buf.sampleRate <= 0.0 || windowSec <= 0.0 || hopSec <= 0.0) return out;
    const size_t win = std::max<size_t>(1, (size_t)std::llround(windowSec * buf.sampleRate));
    const size_t hop = std::max<size_t>(1, (size_t)std::llround(hopSec * buf.sampleRate));
    if (buf.frames < win) {
        AmbiFieldWindow w;
        w.timeSec = 0.0;
        w.field = analyzeAmbisonicRange(buf, 0, buf.frames, order, isN3D, rESpeakerCount);
        out.push_back(std::move(w));
        return out;
    }
    for (size_t s = 0; s + win <= buf.frames; s += hop) {
        AmbiFieldWindow w;
        w.timeSec = (double)s / buf.sampleRate;
        w.field = analyzeAmbisonicRange(buf, s, s + win, order, isN3D, rESpeakerCount);
        out.push_back(std::move(w));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Decode coverage — decode an ambisonic scene to a set of virtual-speaker directions and
// measure how evenly the soundfield's energy is spread over them. The spatial complement of the
// analysis.spatial_field DoA: instead of "where is the dominant source", it answers "how well is the
// whole sphere covered — are there directional holes, is the mix front-heavy or missing height?".
// Decodes with the same real SN3D spherical harmonics used for rE (higher order => sharper
// directivity), so a HOA scene resolves finer coverage than a first-order one.
//
// Basic projection (sampling) decode: for speaker s at direction d_s,
//   g_s(i) = Sum_k a_k(i) * Y_k^SN3D(d_s),   E_s = Sum_i g_s(i)^2.
// Coverage statistics are computed on the per-speaker ENERGY FRACTIONS (E_s / Sum E), so they are
// invariant to overall level. Everything here is pure/std and unit-tested in unit.meter.
// ------------------------------------------------------------------------------------------------

// N3D input -> SN3D per-channel scalar (divide an N3D channel k by sqrt(2l+1) to get SN3D).
inline double n3dToSn3dScale(int k) {
    int l = (int)std::floor(std::sqrt((double)k) + 1e-9);
    return 1.0 / std::sqrt(2.0 * (double)l + 1.0);
}

// Decode a frame range [frameBegin, frameEnd) to `speakers` directions; return per-speaker energy
// (sum of squared decoded gain over the range). `order<=0` => infer from channels; `isN3D` converts
// the channels to SN3D first. Speaker directions use the internal right-handed frame (x=right,
// y=front, z=up) — the same frame fibonacciSphere() and vecToAzEl() speak.
inline std::vector<double> decodeSpeakerEnergies(const AudioBuffer& buf, size_t frameBegin,
                                                 size_t frameEnd, const std::vector<Dir>& speakers,
                                                 int order = 0, bool isN3D = false) {
    std::vector<double> E(speakers.size(), 0.0);
    if (buf.channels < 4 || speakers.empty()) return E;
    int ord = (order > 0) ? order : ambiOrderFromChannels(buf.channels);
    if (ord < 1) ord = 1;
    const int acnUsed = std::min(buf.channels, (ord + 1) * (ord + 1));
    if (frameEnd > buf.frames) frameEnd = buf.frames;
    if (frameBegin >= frameEnd || acnUsed < 4) return E;

    // Precompute per-speaker SH gains (ambiX frame) + per-channel N3D->SN3D scale.
    std::vector<std::vector<double>> shg(speakers.size(), std::vector<double>(acnUsed, 0.0));
    for (size_t s = 0; s < speakers.size(); ++s) {
        const double axf = speakers[s].y, ayl = -speakers[s].x, azu = speakers[s].z;  // -> ambiX axes
        const double azAmbi = std::atan2(ayl, axf);
        const double elAmbi = std::atan2(azu, std::hypot(axf, ayl));
        for (int k = 0; k < acnUsed; ++k) shg[s][k] = realSHsn3d(k, azAmbi, elAmbi);
    }
    std::vector<double> scale(acnUsed, 1.0);
    if (isN3D) for (int k = 0; k < acnUsed; ++k) scale[k] = n3dToSn3dScale(k);

    for (size_t i = frameBegin; i < frameEnd; ++i) {
        for (size_t s = 0; s < speakers.size(); ++s) {
            double g = 0.0;
            for (int k = 0; k < acnUsed; ++k) g += (double)buf.at(i, k) * scale[k] * shg[s][k];
            E[s] += g * g;
        }
    }
    return E;
}

struct CoverageStats {
    bool valid = false;
    int speakers = 0;
    double meanFrac = 0, minFrac = 0, maxFrac = 0, cv = 0;   // energy-fraction distribution
    double uniformity = 0;        // normalized Shannon entropy of the fractions [0,1]; 1 = perfectly even
    double concentrationTop = 0;  // energy fraction carried by the loudest ceil(10%) of directions
    int deadZones = 0;            // directions below deadZoneFrac * mean energy
    // hemisphere energy fractions of the total (listener frame: x=right, y=front, z=up)
    double frontFrac = 0, backFrac = 0, leftFrac = 0, rightFrac = 0, upperFrac = 0, lowerFrac = 0;
    double horizFrac = 0;         // |el| <= 15 deg band (ear-level energy)
    int dominantIdx = -1;
    double dominantAzDeg = 0, dominantElDeg = 0;
};

// Aggregate per-speaker energies (parallel to `dirs`) into coverage statistics. `deadZoneFrac` is the
// fraction of the MEAN energy below which a direction counts as a hole (default 0.1). Pure/testable.
inline CoverageStats computeCoverageStats(const std::vector<Dir>& dirs, const std::vector<double>& E,
                                          double deadZoneFrac = 0.1) {
    CoverageStats c;
    const size_t n = std::min(dirs.size(), E.size());
    if (n == 0) return c;
    c.speakers = (int)n;
    double tot = 0.0;
    for (size_t s = 0; s < n; ++s) tot += E[s];
    if (tot <= 1e-30) return c;   // silent / no energy -> invalid (caller reports a warning)
    c.valid = true;

    const double mean = 1.0 / (double)n;   // fractions sum to 1 => mean fraction = 1/n
    c.meanFrac = mean;
    c.minFrac = 1e9; c.maxFrac = 0.0;
    std::vector<double> frac(n);
    double var = 0.0, ent = 0.0;
    for (size_t s = 0; s < n; ++s) {
        const double p = E[s] / tot; frac[s] = p;
        if (p < c.minFrac) c.minFrac = p;
        if (p > c.maxFrac) { c.maxFrac = p; c.dominantIdx = (int)s; }
        var += (p - mean) * (p - mean);
        if (p > 1e-30) ent += -p * std::log(p);
    }
    var /= (double)n;
    c.cv = (mean > 0.0) ? std::sqrt(var) / mean : 0.0;
    c.uniformity = (n > 1) ? clampd(ent / std::log((double)n), 0.0, 1.0) : 1.0;

    for (size_t s = 0; s < n; ++s) if (frac[s] < deadZoneFrac * mean) c.deadZones++;

    std::vector<double> sorted = frac;
    std::sort(sorted.begin(), sorted.end(), [](double a, double b) { return a > b; });
    size_t topN = (size_t)std::ceil(0.1 * (double)n); if (topN < 1) topN = 1;
    for (size_t s = 0; s < topN && s < n; ++s) c.concentrationTop += sorted[s];

    for (size_t s = 0; s < n; ++s) {
        const double p = frac[s]; const Dir& d = dirs[s];
        if (d.y > 0) c.frontFrac += p; else if (d.y < 0) c.backFrac += p;
        if (d.x < 0) c.leftFrac += p; else if (d.x > 0) c.rightFrac += p;
        if (d.z > 0) c.upperFrac += p; else if (d.z < 0) c.lowerFrac += p;
        const double el = rad2deg(std::atan2(d.z, std::hypot(d.x, d.y)));
        if (std::fabs(el) <= 15.0) c.horizFrac += p;
    }
    if (c.dominantIdx >= 0) vecToAzEl(dirs[(size_t)c.dominantIdx], c.dominantAzDeg, c.dominantElDeg);
    return c;
}

}  // namespace meter
}  // namespace reaper_mcp
