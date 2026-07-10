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
    const double G = 3.99984385397;   // dB
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
    bq.b0 = 1.0 / a0;
    bq.b1 = -2.0 / a0;
    bq.b2 = 1.0 / a0;
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
// True peak (dBTP) — 4x oversampling per BS.1770-4 Annex 2, using a windowed-sinc polyphase FIR.
// ------------------------------------------------------------------------------------------------

// Build a 4-phase polyphase interpolation filter (tapsPerPhase taps/phase, Hann-windowed sinc).
inline std::vector<std::vector<double>> truePeakPolyphase(int tapsPerPhase = 12) {
    const int OS = 4;
    std::vector<std::vector<double>> ph(OS, std::vector<double>(tapsPerPhase, 0.0));
    const int half = tapsPerPhase / 2;
    for (int p = 0; p < OS; ++p) {
        const double frac = (double)p / OS;
        for (int t = 0; t < tapsPerPhase; ++t) {
            const double n = (double)(t - half + 1) - frac;  // sinc argument in input samples
            double s = (std::fabs(n) < 1e-9) ? 1.0 : std::sin(M_PI * n) / (M_PI * n);
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (t + 0.5) / tapsPerPhase);  // Hann
            ph[p][t] = s * w;
        }
    }
    return ph;
}

inline double truePeakDb(const AudioBuffer& buf, int ch) {
    static const std::vector<std::vector<double>> ph = truePeakPolyphase();
    const int OS = 4, taps = (int)ph[0].size(), half = taps / 2;
    double peak = 0.0;
    for (size_t i = 0; i < buf.frames; ++i) {
        // phase 0 == the input sample itself (exact); phases 1..3 interpolate between samples.
        double s0 = std::fabs((double)buf.at(i, ch));
        if (s0 > peak) peak = s0;
        for (int p = 1; p < OS; ++p) {
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
    return linToDb(peak);
}

// ------------------------------------------------------------------------------------------------
// Per-channel metrics + inter-channel correlation
// ------------------------------------------------------------------------------------------------

struct ChannelMetrics {
    double rmsDb = kMinDb();
    double peakDb = kMinDb();
    double truePeakDb = kMinDb();
    double kLevelLkfs = kMinDb();  // ungated K-weighted level (per-channel loudness contribution)
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
    m.truePeakDb = truePeakDb(buf, ch);
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
// timeline metrics. Channel weighting follows BS.1770-4: G = 1.0 for front channels, 1.41 for
// surrounds, LFE excluded — the tool layer derives the weight vector from the SMPTE bed labels and
// passes it in (empty weights = all 1.0).
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
