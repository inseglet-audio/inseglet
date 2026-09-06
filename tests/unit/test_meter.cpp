// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_meter.cpp — pure unit test for the Batch M1 immersive-metering DSP (src/ambisonic_meter.h):
// WAV round-trip, per-channel level/peak/true-peak/K-weighting, inter-channel correlation, and the
// ambisonic field (DirAC intensity DoA + diffuseness, Gerzon rV/rE localization vectors). No REAPER,
// no SDK — synthetic buffers only, so every number here is deterministic.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <filesystem>

#include "ambisonic_meter.h"

using namespace reaper_mcp::meter;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
// Circular degree difference in [-180,180], so 179 vs -179 is 2 deg apart.
static double angDiff(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
    return std::fabs(d);
}

// ---- helpers to synthesize buffers ----
static AudioBuffer makeBuffer(int ch, double sr, size_t frames) {
    AudioBuffer b; b.channels = ch; b.sampleRate = sr; b.frames = frames;
    b.samples.assign(frames * (size_t)ch, 0.0f);
    return b;
}
static void setSample(AudioBuffer& b, size_t frame, int ch, double v) {
    b.samples[frame * (size_t)b.channels + ch] = (float)v;
}

// (az,el) deg -> ambiX SH azimuth/elevation (rad). Azimuth is the ambisonic-standard convention:
// CCW from front, +az = LEFT — the same frame analyzeAmbisonic reports its DoA/rV/rE in, and that
// spatial.ambisonic_encode uses, so encode(theta) round-trips to a reported DoA azimuth of theta.
static void ambiAzElToRad(double azDeg, double elDeg, double& azAmbi, double& elAmbi) {
    azAmbi = azDeg * M_PI / 180.0;
    elAmbi = elDeg * M_PI / 180.0;
}

// Encode a mono signal s(t) at ambisonic direction (az,el) [+az = left, CCW] into an ACN/SN3D buffer.
static AudioBuffer encodeSource(double azDeg, double elDeg, int order, double sr, size_t frames,
                                double freq = 440.0, double amp = 0.5) {
    int nch = (order + 1) * (order + 1);
    AudioBuffer b = makeBuffer(nch, sr, frames);
    double azAmbi, elAmbi; ambiAzElToRad(azDeg, elDeg, azAmbi, elAmbi);
    std::vector<double> g(nch);
    for (int k = 0; k < nch; ++k) g[k] = realSHsn3d(k, azAmbi, elAmbi);
    for (size_t i = 0; i < frames; ++i) {
        double s = amp * std::sin(2.0 * M_PI * freq * (double)i / sr);
        for (int k = 0; k < nch; ++k) setSample(b, i, k, s * g[k]);
    }
    return b;
}

// Minimal WAV writers (to exercise readWavFile round-trip).
static void wrU32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}
static void wrU16(std::vector<unsigned char>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static bool writeWav(const std::string& path, const AudioBuffer& b, bool asFloat) {
    std::vector<unsigned char> d;
    int bits = asFloat ? 32 : 16;
    int bytes = bits / 8;
    uint32_t dataBytes = (uint32_t)(b.frames * (size_t)b.channels * (size_t)bytes);
    uint16_t fmtTag = asFloat ? 3 : 1;
    for (char c : std::string("RIFF")) d.push_back(c);
    wrU32(d, 36 + dataBytes);
    for (char c : std::string("WAVE")) d.push_back(c);
    for (char c : std::string("fmt ")) d.push_back(c);
    wrU32(d, 16);
    wrU16(d, fmtTag); wrU16(d, (uint16_t)b.channels);
    wrU32(d, (uint32_t)b.sampleRate);
    wrU32(d, (uint32_t)(b.sampleRate * b.channels * bytes));
    wrU16(d, (uint16_t)(b.channels * bytes)); wrU16(d, (uint16_t)bits);
    for (char c : std::string("data")) d.push_back(c);
    wrU32(d, dataBytes);
    for (size_t i = 0; i < b.frames * (size_t)b.channels; ++i) {
        double v = b.samples[i];
        if (asFloat) { float f = (float)v; unsigned char* p = (unsigned char*)&f; for (int k = 0; k < 4; ++k) d.push_back(p[k]); }
        else { int t = (int)std::lround(v * 32767.0); if (t > 32767) t = 32767; if (t < -32768) t = -32768; wrU16(d, (uint16_t)(int16_t)t); }
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    return true;
}

// Portable scratch path — Windows/MSVC has no /tmp, so route the WAV round-trip scratch files through
// the OS temp directory instead of a hardcoded POSIX path (a failed write there left the read buffer
// empty and r.at() dereferenced out of bounds → segfault on the Windows CI). Non-throwing; CWD fallback.
static std::string tmpPath(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::path(".");
    return (dir / name).string();
}

int main() {
    const double sr = 48000.0;
    const size_t N = 4800;  // 0.1 s

    // ---- 1. WAV round-trip (float32 + PCM16) ----
    {
        AudioBuffer b = makeBuffer(2, sr, 1000);
        for (size_t i = 0; i < 1000; ++i) {
            setSample(b, i, 0, 0.5 * std::sin(2 * M_PI * 1000 * i / sr));
            setSample(b, i, 1, -0.25);
        }
        check(writeWav(tmpPath("_mcp_meter_f32.wav"), b, true), "write float32 WAV");
        AudioBuffer r; std::string err;
        check(readWavFile(tmpPath("_mcp_meter_f32.wav"), r, err), "read float32 WAV");
        check(r.channels == 2 && r.frames == 1000 && near(r.sampleRate, sr, 1),
              "float32 WAV header round-trips (ch/frames/sr)");
        check(near(r.at(10, 1), -0.25, 1e-6), "float32 sample value exact");

        check(writeWav(tmpPath("_mcp_meter_i16.wav"), b, false), "write PCM16 WAV");
        AudioBuffer r2; check(readWavFile(tmpPath("_mcp_meter_i16.wav"), r2, err), "read PCM16 WAV");
        check(r2.channels == 2 && r2.frames == 1000, "PCM16 WAV header round-trips");
        check(near(r2.at(10, 1), -0.25, 1e-3), "PCM16 sample value within quantization");
        AudioBuffer bad; check(!readWavFile(tmpPath("_does_not_exist_.wav"), bad, err), "missing file fails cleanly");
    }

    // ---- 2. per-channel level, peak, and +6 dB doubling ----
    {
        AudioBuffer b = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(b, i, 0, 1.0 * std::sin(2 * M_PI * 1000 * i / sr));
        ChannelMetrics m = analyzeChannel(b, 0);
        check(near(m.peakDb, 0.0, 0.1), "full-scale 1kHz sine: peak ~= 0 dBFS");
        check(near(m.rmsDb, -3.01, 0.1), "sine RMS ~= -3.01 dBFS");

        AudioBuffer h = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(h, i, 0, 0.5 * std::sin(2 * M_PI * 1000 * i / sr));
        ChannelMetrics mh = analyzeChannel(h, 0);
        check(near(m.rmsDb - mh.rmsDb, 6.02, 0.05), "halving amplitude drops RMS by ~6 dB");

        AudioBuffer sil = makeBuffer(1, sr, N);
        ChannelMetrics ms = analyzeChannel(sil, 0);
        check(ms.peakDb <= -100.0, "silence reports floor peak");
    }

    // ---- 3. true peak catches inter-sample peaks ----
    {
        // A cosine at fs/4 sampled with a 45-degree phase offset: samples are +/-0.7071 (sample peak
        // -3 dBFS) but the reconstructed waveform peaks at 1.0 (~0 dBTP). Classic BS.1770 TP test.
        AudioBuffer b = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i)
            setSample(b, i, 0, std::cos(2 * M_PI * (sr / 4.0) * i / sr + M_PI / 4.0));
        ChannelMetrics m = analyzeChannel(b, 0);
        check(near(m.peakDb, -3.01, 0.2), "inter-sample signal: sample peak ~= -3 dBFS");
        check(m.truePeakDb > m.peakDb + 2.0, "true peak exceeds sample peak by >2 dB (catches ISP)");
        check(m.truePeakDb > -1.0, "true peak of the inter-sample signal approaches 0 dBTP");
    }

    // ---- 3b. true peak against EBU Tech 3341 Table 1, signals 15-19 (+0.2 / -0.4 dBTP) ----
    {
        // The five true-peak "minimum requirements" signals: sines at fs/4, fs/6 and fs/8 with phases
        // chosen so the sample grid never lands on the peak, tapered 10 ms in and out as Tech 3341
        // specifies (a raised cosine). The +0.2/-0.4 tolerance is Tech 3341's, not ours. Signal 19
        // exceeds full scale on purpose: a true-peak meter must read above 0 dBTP.
        struct TP { const char* id; double fOverFs, amp, phaseDeg, expectDbTP; } cases[] = {
            {"15", 0.25,    0.50, 0.0,  -6.0}, {"16", 0.25,  0.50, 45.0, -6.0}, {"17", 1.0 / 6, 0.50, 60.0, -6.0},
            {"18", 0.125,   0.50, 67.5, -6.0}, {"19", 0.25,  1.41, 45.0,  3.0}};
        const size_t nf = (size_t)(sr * 0.010);
        for (const auto& c : cases) {
            AudioBuffer b = makeBuffer(1, sr, N);
            for (size_t i = 0; i < N; ++i) {
                double g = 1.0;
                if (i < nf)          g = 0.5 - 0.5 * std::cos(M_PI * (double)i / (double)nf);
                else if (i >= N - nf) g = 0.5 - 0.5 * std::cos(M_PI * (double)(N - 1 - i) / (double)nf);
                setSample(b, i, 0, g * c.amp * std::sin(2 * M_PI * c.fOverFs * (double)i + c.phaseDeg * M_PI / 180.0));
            }
            ChannelMetrics m = analyzeChannel(b, 0);
            const double err = m.truePeakDb - c.expectDbTP;
            check(err >= -0.4 && err <= 0.2, std::string("Tech 3341 signal ") + c.id +
                  ": true peak within +0.2/-0.4 dBTP of " + std::to_string(c.expectDbTP) + " (err " + std::to_string(err) + ")");
            check(m.truePeakDb >= m.peakDb - 1e-9, std::string("Tech 3341 signal ") + c.id + ": true peak is never below the sample peak");
        }
    }

    // ---- 3c. the edge guard, and the invariant it does NOT inherit ----
    {
        // The interpolator is fed ZEROS outside the buffer, so a buffer whose first or last sample
        // is non-zero presents it with a step it rings at.  truePeakDb keeps reporting that ring;
        // truePeakDbInterior is the same maximum with kTruePeakEdgeGuard samples excluded at BOTH
        // ends, and truePeakEdgeDominated says the whole-buffer maximum came only from a guard.
        //
        // The fixture is Tech 3341 signal 18 (0.125 fs, 0.5, 67.5 deg), the case built and
        // measured earlier.  ITS LENGTH IS 4802, NOT 4800, ON PURPOSE: 4800 is 600 WHOLE
        // periods of an 8-sample signal, which reads the TAIL ring as exactly zero and hid it from
        // the first instrument used here.  A test on a whole-period length would pass while measuring
        // one edge and calling it both.
        const size_t NE = 4802, nf18 = (size_t)(sr * 0.010);
        auto sig18 = [&](bool fadeHead, bool fadeTail) {
            AudioBuffer b = makeBuffer(1, sr, NE);
            for (size_t i = 0; i < NE; ++i) {
                double g = 1.0;
                if (fadeHead && i < nf18)        g = 0.5 - 0.5 * std::cos(M_PI * (double)i / (double)nf18);
                if (fadeTail && i >= NE - nf18)  g = 0.5 - 0.5 * std::cos(M_PI * (double)(NE - 1 - i) / (double)nf18);
                setSample(b, i, 0, g * 0.5 * std::sin(2 * M_PI * 0.125 * (double)i + 67.5 * M_PI / 180.0));
            }
            return b;
        };
        check(NE % 8 != 0, "edge-guard fixture length is NOT a whole number of periods (d266 C-5)");

        // ONE VARIABLE: the same signal, tapers on and off.
        AudioBuffer raw = sig18(false, false), tap = sig18(true, true);
        ChannelMetrics mr = analyzeChannel(raw, 0), mt = analyzeChannel(tap, 0);
        check(mr.truePeakInteriorValid && mt.truePeakInteriorValid, "edge guard: both fixtures have an interior");
        check(mr.truePeakEdgeDominated,  "raw-edged buffer: truePeakEdgeDominated is TRUE");
        check(!mt.truePeakEdgeDominated, "tapered buffer: truePeakEdgeDominated is FALSE");
        // ⛔ THE RING IS A PROPERTY OF THE TABLE.  With the Recommendation's 4x12 table (through 1.20.0)
        //    this fixture read +0.7096 dB; with the 8x24 table it reads +0.6954 dB -- measured through the
        //    product on the day the table changed, and the first build of that change found the old datum
        //    by failing here, which is what a pinned datum is for.  Re-pinned, not loosened.
        check(near(mr.truePeakDb - mr.truePeakDbInterior, 0.6954, 0.01),
              "raw-edged buffer: the ring the two numbers disclose is ~+0.70 dB for the 8x24 table (was +0.71 at 4x12)");
        check(near(mt.truePeakDb - mt.truePeakDbInterior, 0.0, 1e-9),
              "tapered buffer: the two numbers agree exactly");

        // BOTH ENDS, SEPARATELY.  The earlier correction's second half: the tail is not the lesser case.
        AudioBuffer headRaw = sig18(false, true), tailRaw = sig18(true, false);
        ChannelMetrics mh = analyzeChannel(headRaw, 0), mtl = analyzeChannel(tailRaw, 0);
        check(mh.truePeakEdgeDominated,  "head raw / tail tapered: flag TRUE");
        check(mtl.truePeakEdgeDominated, "tail raw / head tapered: flag TRUE");
        check(near(mh.truePeakDb, mtl.truePeakDb, 1e-9),
              "both edges ring by the SAME amount, at a non-whole-period length");

        // The interior is a maximum over a SUBSET and can never exceed the maximum over the whole.
        for (const ChannelMetrics* m : {&mr, &mt, &mh, &mtl})
            check(m->truePeakDbInterior <= m->truePeakDb + 1e-12,
                  "truePeakDbInterior never exceeds truePeakDb");

        // NO INTERIOR: a buffer of exactly 2*guard frames has none, and the flag says so rather
        // than a floor value standing in for a reading.
        AudioBuffer tiny = makeBuffer(1, sr, (size_t)(2 * kTruePeakEdgeGuard));
        for (size_t i = 0; i < tiny.frames; ++i) setSample(tiny, i, 0, (i % 2) ? -0.5 : 0.5);
        ChannelMetrics mtiny = analyzeChannel(tiny, 0);
        check(!mtiny.truePeakInteriorValid, "a buffer of 2*guard frames has NO interior");
        check(!mtiny.truePeakEdgeDominated, "no interior => the flag is not set either");
        AudioBuffer least = makeBuffer(1, sr, (size_t)(2 * kTruePeakEdgeGuard + 1));
        for (size_t i = 0; i < least.frames; ++i) setSample(least, i, 0, (i % 2) ? -0.5 : 0.5);
        check(analyzeChannel(least, 0).truePeakInteriorValid,
              "a buffer of 2*guard+1 frames DOES have an interior");

        // ⛔ THE INVARIANT THAT DOES NOT TRANSFER, ASSERTED AS INTENDED RATHER THAN LEFT TO SURPRISE.
        //   Section 3b checks `truePeakDb >= peakDb` for every Tech 3341 signal, and that promise is
        //   why the raw sample is kept as a floor.  truePeakDbInterior CANNOT keep it: a guarded
        //   position is excluded entirely, sample and all.  A lone full-scale sample at index 0 is
        //   the sharpest case, and this test exists so nobody "fixes" the field into breaking the
        //   definition that was actually measured.
        AudioBuffer lone = makeBuffer(1, sr, 100);
        setSample(lone, 0, 0, 1.0);
        ChannelMetrics ml = analyzeChannel(lone, 0);
        check(near(ml.peakDb, 0.0, 1e-9), "lone full-scale sample at index 0: sample peak is 0 dBFS");
        check(ml.truePeakInteriorValid && ml.truePeakDbInterior < ml.peakDb - 10.0,
              "BY DESIGN: truePeakDbInterior is NOT bounded below by the sample peak");
        check(ml.truePeakEdgeDominated, "lone edge sample: the flag names it");
    }

    // ---- 3c-bis. the fused pass is BIT-IDENTICAL to the two passes it replaced ----
    {
        // analyzeChannel() now folds every position into both maxima in ONE pass of the interpolator.
        // The two-pass functions it replaced are still public; this asserts EXACT equality (==, not a
        // tolerance) between the fused numbers and theirs over fixtures that exercise every branch:
        // the five Tech 3341 sines, a raw-edged sine, a lone edge sample, noise, silence, a buffer with
        // no interior and one with the least interior.  A tolerance here would hide a fuse that was
        // merely close; the claim is that it is the same arithmetic.
        std::vector<std::pair<std::string, AudioBuffer>> fx;
        const size_t nf = (size_t)(sr * 0.010);
        struct TP { const char* id; double fOverFs, amp, phaseDeg; } cases[] = {
            {"15", 0.25, 0.50, 0.0}, {"16", 0.25, 0.50, 45.0}, {"17", 1.0 / 6, 0.50, 60.0},
            {"18", 0.125, 0.50, 67.5}, {"19", 0.25, 1.41, 45.0}};
        for (const auto& c : cases) {
            AudioBuffer b = makeBuffer(1, sr, N);
            for (size_t i = 0; i < N; ++i) {
                double g = 1.0;
                if (i < nf)          g = 0.5 - 0.5 * std::cos(M_PI * (double)i / (double)nf);
                else if (i >= N - nf) g = 0.5 - 0.5 * std::cos(M_PI * (double)(N - 1 - i) / (double)nf);
                setSample(b, i, 0, g * c.amp * std::sin(2 * M_PI * c.fOverFs * (double)i + c.phaseDeg * M_PI / 180.0));
            }
            fx.emplace_back(std::string("3341-") + c.id, b);
        }
        { AudioBuffer b = makeBuffer(1, sr, N); for (size_t i = 0; i < N; ++i) setSample(b, i, 0, 0.5 * std::sin(2 * M_PI * 0.125 * (double)i + 67.5 * M_PI / 180.0)); fx.emplace_back("raw-edged sine", b); }
        { AudioBuffer b = makeBuffer(1, sr, N); setSample(b, 0, 0, 1.0); fx.emplace_back("lone edge sample", b); }
        { AudioBuffer b = makeBuffer(1, sr, N); uint32_t r = 12345u; for (size_t i = 0; i < N; ++i) { r = r * 1664525u + 1013904223u; setSample(b, i, 0, 0.5 * ((double)(r >> 8) / 16777216.0 * 2.0 - 1.0)); } fx.emplace_back("noise", b); }
        { AudioBuffer b = makeBuffer(1, sr, N); fx.emplace_back("silence", b); }
        { AudioBuffer b = makeBuffer(1, sr, (size_t)(2 * kTruePeakEdgeGuard)); for (size_t i = 0; i < b.frames; ++i) setSample(b, i, 0, (i % 2) ? -0.5 : 0.5); fx.emplace_back("no interior", b); }
        { AudioBuffer b = makeBuffer(1, sr, (size_t)(2 * kTruePeakEdgeGuard + 1)); for (size_t i = 0; i < b.frames; ++i) setSample(b, i, 0, (i % 2) ? -0.5 : 0.5); fx.emplace_back("least interior", b); }
        int identical = 0;
        for (const auto& f : fx) {
            ChannelMetrics m = analyzeChannel(f.second, 0);
            const double two = truePeakDb(f.second, 0);
            double twoInterior = kMinDb(); const bool twoValid = truePeakDbInterior(f.second, 0, twoInterior);
            const bool same = (m.truePeakDb == two) && (m.truePeakInteriorValid == twoValid) &&
                              (!twoValid || m.truePeakDbInterior == twoInterior);
            check(same, "fused pass == two passes, bit-identical, on fixture '" + f.first + "'");
            if (same) ++identical;
        }
        check(identical == (int)fx.size(), "fused pass identical on ALL " + std::to_string(fx.size()) + " fixtures");
        // NEGATIVE, so the equality above is not a clean null: the guarded interior of the raw-edged
        // sine is BELOW the whole-buffer reading (the edge ring), i.e. the two numbers the fuse
        // computes are genuinely different quantities, not one number copied twice.
        ChannelMetrics mr = analyzeChannel(fx[5].second, 0);
        check(mr.truePeakInteriorValid && mr.truePeakDbInterior < mr.truePeakDb - 0.1,
              "fused: the interior and whole-buffer maxima DIFFER on the raw-edged fixture (not one number twice)");
    }

    // ---- 3d. the estimator DISCLOSES its own ceiling ----
    {
        // The 4x estimator evaluates the reconstructed waveform on a grid of 1/OS of a sample, so
        // the nearest point it looks at is at most 1/(2*OS) away from a real inter-sample maximum.
        // truePeakGridBoundDb is how far a full-band sine peak can therefore fall BELOW the truth.
        // ⛔ THIS TEST DOES NOT RE-DERIVE THE FORMULA -- that would just assert the code against
        //    itself.  It asserts three things the code could get wrong INDEPENDENTLY:
        const double b = truePeakGridBoundDb();
        //  (a) it agrees with the closed form to the last bit a double carries.
        const double closed = -20.0 * std::log10(std::cos(M_PI / (2.0 * (double)kTruePeakOversampling)));
        check(std::fabs(b - closed) < 1e-12, "grid bound == -20 log10 cos(pi/(2*OS))");
        //  (b) it is POSITIVE and SMALL -- a sign error here would tell a user the meter reads high.
        check(b > 0.0 && b < 3.0, "grid bound is a positive, small number of dB");
        //  (c) THE MEASURED LOW SIDE SITS INSIDE grid + filter loss, FOR THIS TABLE.  Two data, both in
        //      context against an exact reference on the EBU's published files: the fs/4 tones (Tech
        //      3341 cases 16/19) read 0.126620 dB low; castanets (SQAM 27, content at the passband edge)
        //      reads 0.136516 dB low.  The LOW side is the grid bound at the signal's frequency PLUS the
        //      filter's worst per-phase loss over the passband -- the loss is computed HERE from the
        //      table (max over phases and 0..0.45 fs of -20 log10 |H_p|), not read from the header.
        //      If a future table made grid + loss tighter than an error already observed, the
        //      disclosure would be a false reassurance, which is worse than none.
        {
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2;
            double lossDb = 0.0;
            for (int k = 0; k <= 900; ++k) {
                const double f = 0.45 * k / 900.0, w = 2 * M_PI * f;
                for (const auto& hp : ph) { double re = 0, im = 0;
                    for (int tt = 0; tt < taps; ++tt) { const double m = tt - half + 1; re += hp[tt] * std::cos(w * m); im += hp[tt] * std::sin(w * m); }
                    lossDb = std::max(lossDb, -20.0 * std::log10(std::sqrt(re * re + im * im))); }
            }
            const double gridQuarter = -20.0 * std::log10(std::cos(2.0 * M_PI * 0.25 / (2.0 * (double)kTruePeakOversampling)));
            const double gridEdge    = -20.0 * std::log10(std::cos(2.0 * M_PI * 0.45 / (2.0 * (double)kTruePeakOversampling)));
            check(lossDb > 0.0 && lossDb < 0.2, "the filter's worst per-phase passband loss is positive and small (computed from the table)");
            check(gridQuarter + lossDb >= 0.126620, "grid(fs/4) + filter loss covers the measured fs/4 tone shortfall (cases 16/19)");
            check(gridEdge + lossDb >= 0.136516, "grid(0.45 fs) + filter loss covers the measured castanets shortfall (SQAM 27)");
            check(gridEdge <= 0.136516 + 0.001, "castanets reads AT the grid bound: an accurate filter leaves the grid (within 0.001 dB)");
        }
        // (d) the phase table really has OS rows -- the loop's factor and the disclosed factor are
        //     the same number, which is the drift this constant exists to prevent.
        check((int)truePeakPolyphase().size() == kTruePeakOversampling,
              "the polyphase table has kTruePeakOversampling rows");
        // ---- 3d, continued: the OTHER side of the bound ----
        //  (e) the filter-gain bound is positive and small, and smaller than the grid bound: a sign
        //      error would tell a user the filter attenuates; a large value would mean the table is not
        //      the Recommendation's.
        const double g = truePeakFilterGainBoundDb();
        check(g > 0.0 && g < b, "filter-gain bound is positive and smaller than the grid bound");
        //  (f) THE BOUND IS TIGHT AGAINST THE METER ITSELF -- a sine at the frequency where the table's
        //      gain peaks, driven through truePeakDb at 32 sub-sample alignments, reads within 0.002 dB
        //      of truth + bound, and never above it.  A literal that drifted from the table would fail
        //      this; a bound that was not the table's maximum would fail this.  The frequency is found
        //      here by scanning the table a second way (coarse), not read from the function under test.
        {
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2;
            double bestG = 0.0, bestF = 0.0;
            for (int k = 0; k <= 2000; ++k) {
                const double f = 0.5 * k / 2000.0, w = 2 * M_PI * f; double gg = 0.0;
                for (const auto& hp : ph) { double re = 0, im = 0;
                    for (int tt = 0; tt < taps; ++tt) { const double m = tt - half + 1; re += hp[tt] * std::cos(w * m); im += hp[tt] * std::sin(w * m); }
                    gg = std::max(gg, std::sqrt(re * re + im * im)); }
                if (gg > bestG) { bestG = gg; bestF = f; }
            }
            double worst = -1e9, worstWhole = -1e9;
            for (int o = 0; o < 32; ++o) {
                AudioBuffer sn = makeBuffer(1, sr, 4096);
                for (size_t i = 0; i < 4096; ++i) setSample(sn, i, 0, std::sin(2 * M_PI * bestF * i + 2 * M_PI * o / 32.0));
                double interior = 0.0;
                const bool valid = truePeakDbInterior(sn, 0, interior);
                check(valid, "the interior reading is valid on a 4096-sample buffer");
                worst = std::max(worst, interior);                   // truth is 0 dBFS
                worstWhole = std::max(worstWhole, truePeakDb(sn, 0));
            }
            check(worst <= g + 1e-6, "no sine over-reads the filter-gain bound through the INTERIOR reading (32 alignments at the worst frequency)");
            check(worst >= g - 0.002, "the filter-gain bound is TIGHT: the worst-frequency sine reaches it within 0.002 dB");
            //  (f') THE SCOPE, AS A TEST: the WHOLE-buffer reading CAN exceed the bound, because at the buffer's
            //       edges the taps are fed zeros and ring at the step.  The first build of this test
            //       asserted the bound through truePeakDb() and FAILED -- correctly.  The bound is about the
            //       filter on the signal; an edge is not the signal, and truePeakEdgeDominated is the flag.
            check(worstWhole > worst, "SCOPE: the whole-buffer reading exceeds the interior at a raw edge, so the bound is for the interior");
        }
        //  (g) THE MEASURED PROGRAMME OVER-READ, FOR THIS TABLE, AND WHAT THE SCOPE SENTENCE SAYS ABOUT IT:
        //      SQAM 64 read +0.096820 dB high in context against an exact reference -- ABOVE the sine
        //      bound, because a broadband transient is not a sine (the phases' phase error can align
        //      components better than the input had them).  The description says the excess exists and
        //      is slight; this asserts BOTH halves, so the sentence is neither vacuous nor understated.
        //      (For the 4x table the datum was +0.215802 on Euroradio 05, inside its +0.222 bound.)
        check(0.096820 > g, "the measured programme over-read (SQAM 64) EXCEEDS the sine bound -- the scope sentence is not vacuous");
        check(0.096820 - g < 0.02, "and the excess is slight (< 0.02 dB), as the description says");
        // ---- 3d, continued: the THIRD number -- the filter's LOSS, as a field ----
        //  (h) sign, size and place: the loss bound is positive (a sign error would say the filter has
        //      gain), small, and smaller than the grid bound for this table; and the field IS the
        //      function of (the shipped table, the shipped edge) -- nothing else feeds it.
        const double lossB = truePeakFilterLossBoundDb();
        check(lossB > 0.0 && lossB < 0.2, "filter-loss bound is positive and small");
        check(lossB < b, "filter-loss bound is smaller than the grid bound, for this table");
        check(std::fabs(lossB - truePeakFilterLossBoundDbOf(truePeakPolyphase(), kTruePeakPassbandEdge)) < 1e-12,
              "the field is the function of the shipped table and the shipped passband edge");
        //  (i) IT AGREES WITH THIS FILE'S OWN INDEPENDENT SCAN -- (c) above computes the same maximum on a
        //      901-point grid; re-derived here so this block is self-contained.  The two scans are written
        //      separately and could disagree; a maximum can only be reached from below by a coarser grid.
        {
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2;
            double lossGrid = 0.0;
            for (int k = 0; k <= 900; ++k) {
                const double f = kTruePeakPassbandEdge * k / 900.0, w = 2 * M_PI * f;
                for (const auto& hp : ph) { double re = 0, im = 0;
                    for (int tt = 0; tt < taps; ++tt) { const double m = tt - half + 1; re += hp[tt] * std::cos(w * m); im += hp[tt] * std::sin(w * m); }
                    lossGrid = std::max(lossGrid, -20.0 * std::log10(std::sqrt(re * re + im * im))); }
            }
            check(lossGrid <= lossB + 1e-9, "the field is never smaller than the coarse independent scan (it is a maximum)");
            check(lossB - lossGrid < 1e-6, "and the coarse scan reaches it within 1e-6 dB (for this table the worst is at the edge)");
        }
        //  (j) THE FIELD COVERS THE MEASURED LOW-SIDE DATA -- (c) proved grid + loss covers them with the loss
        //      computed by the test; this proves the number a USER receives does.
        {
            const double gridQ = -20.0 * std::log10(std::cos(2.0 * M_PI * 0.25 / (2.0 * (double)kTruePeakOversampling)));
            const double gridE = -20.0 * std::log10(std::cos(2.0 * M_PI * kTruePeakPassbandEdge / (2.0 * (double)kTruePeakOversampling)));
            check(gridQ + lossB >= 0.126620, "grid(fs/4) + the FIELD covers the measured fs/4 tone shortfall (cases 16/19)");
            check(gridE + lossB >= 0.136516, "grid(edge) + the FIELD covers the measured castanets shortfall (SQAM 27)");
        }
        //  (k) THE LOW-SIDE BOUND IS TIGHT AGAINST THE METER ITSELF, AT fs/4.  A long sine at fs/4 samples the
        //      same four positions for ever, so a peak that falls midway between two interpolated phases is
        //      never rescued by a later cycle (at any other frequency it is: a long tone defeats a single-peak
        //      bound).  Driven through the INTERIOR reading at 32 alignments (steps of 1/8 sample), the worst
        //      reading is truth - (grid(fs/4) + loss(fs/4)) within 0.002 dB ON EITHER SIDE.  ⛔ EITHER SIDE,
        //      because the first build of this test asserted "never below it" and FAILED -- correctly: the
        //      meter read 0.000831 dB BELOW grid + loss.  grid + loss is an ideal-delay MODEL; the filter's
        //      PHASE error moves each phase's effective sampling instant (phase 3 sits at 0.436884 of a
        //      sample at fs/4, not 0.4375), and that is a THIRD low-side contribution, small here.  The
        //      guarantee the description states is the FIELD-level one, asserted last: the sum of the two
        //      fields covers the true single-peak worst case anywhere in the passband (0.127 dB, at fs/4, for
        //      this table) with room, because the grid bound is taken at fs/2 and the loss bound at the edge.
        {
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2;
            const double w = 2 * M_PI * 0.25; double gmin = 1e300;
            for (const auto& hp : ph) { double re = 0, im = 0;
                for (int tt = 0; tt < taps; ++tt) { const double m = tt - half + 1; re += hp[tt] * std::cos(w * m); im += hp[tt] * std::sin(w * m); }
                gmin = std::min(gmin, std::sqrt(re * re + im * im)); }
            const double lossQ = -20.0 * std::log10(gmin);
            const double gridQ = -20.0 * std::log10(std::cos(2.0 * M_PI * 0.25 / (2.0 * (double)kTruePeakOversampling)));
            check(lossQ <= lossB + 1e-12, "the loss at fs/4 is inside the passband maximum");
            double worst = 1e9;
            for (int o = 0; o < 32; ++o) {
                AudioBuffer sn = makeBuffer(1, sr, 4096);
                for (size_t i = 0; i < 4096; ++i) setSample(sn, i, 0, std::sin(2 * M_PI * 0.25 * i + 2 * M_PI * o / 32.0));
                double interior = 0.0;
                const bool valid = truePeakDbInterior(sn, 0, interior);
                check(valid, "the interior reading is valid on a 4096-sample buffer (fs/4)");
                worst = std::min(worst, interior);                   // truth is 0 dBFS
            }
            check(std::fabs(worst + (gridQ + lossQ)) < 0.002, "at fs/4 the meter's worst alignment is within 0.002 dB of grid + loss, on either side (the residual is the filter's PHASE error)");
            check(worst >= -(b + lossB) - 1e-5, "and the FIELD-level bound (grid bound + loss bound) covers it, as the description promises");
        }
        //  (l) A PLANTED TABLE MOVES IT.  The same table scaled by 0.99 in every coefficient loses a further
        //      20 log10(1/0.99) = 0.0873 dB at every frequency and phase, so the function of (table, edge) must
        //      read the shipped value + 0.0873 within 1e-9; a function that ignored its table, or a literal,
        //      would not move.  And the EDGE is a parameter the constant must agree with: at fs/2 the shipped
        //      table has rolled off by several dB, so a constant that named 0.5 as the edge would report a
        //      "loss bound" that is not a metering error at all -- asserted, so the constant and the table
        //      cannot drift apart without a test noticing.
        {
            auto planted = truePeakPolyphase();
            for (auto& hp : planted) for (auto& coef : hp) coef *= 0.99;
            const double moved = truePeakFilterLossBoundDbOf(planted, kTruePeakPassbandEdge);
            check(std::fabs(moved - (lossB - 20.0 * std::log10(0.99))) < 1e-9, "a planted table (every coefficient x 0.99) MOVES the loss bound by exactly 20 log10(1/0.99)");
            check(truePeakFilterLossBoundDbOf(truePeakPolyphase(), 0.5) > 3.0, "the edge is real: naming fs/2 as the passband edge would read a multi-dB 'loss' (the roll-off), so the constant describes the table");
            check(truePeakFilterLossBoundDbOf(truePeakPolyphase(), kTruePeakPassbandEdge) < 0.2, "and just inside the shipped edge the loss is small");
        }
        // ---- 3d, continued: the FOURTH number -- the tight low side for a steady sine ----
        //  (m) sign, size and place: positive; at most the two fields' sum (it is the tight version of that
        //      sum, not a fifth mechanism); at least grid(f*) + loss(f*) at its own frequency minus a hair (the
        //      phase term can only add for this table -- (k) measured it adding 0.0008 dB); the field IS the
        //      function of (the shipped table, the shipped edge); its frequency is a member of the family.
        const double sineB = truePeakSineLowBoundDb(), fStar = truePeakSineLowBoundFrequency();
        check(sineB > 0.0 && sineB < 0.5, "sine low bound is positive and small");
        check(sineB <= b + lossB + 1e-9, "the tight number is inside the two fields' sum, as the description promises");
        check(sineB < 0.75 * (b + lossB), "and materially tighter than it, for this table (the point of the field)");
        check(std::fabs(sineB - truePeakSineLowBoundOf(truePeakPolyphase(), kTruePeakPassbandEdge).db) < 1e-12, "the field is the function of the shipped table and the shipped passband edge");
        check(fStar > 0.0 && fStar <= kTruePeakPassbandEdge, "the worst frequency is inside the passband");
        check(std::fabs(fStar - 0.25) < 1e-12, "for this table the worst frequency is fs/4 (the standard's own test-tone frequency)");
        {
            const double w = 2 * M_PI * fStar; double gmin = 1e300;
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2;
            for (const auto& hp : ph) { double re = 0, im = 0;
                for (int tt = 0; tt < taps; ++tt) { const double m = tt - half + 1; re += hp[tt] * std::cos(w * m); im += hp[tt] * std::sin(w * m); }
                gmin = std::min(gmin, std::sqrt(re * re + im * im)); }
            const double gridF = -20.0 * std::log10(std::cos(2.0 * M_PI * fStar / (2.0 * (double)kTruePeakOversampling)));
            check(sineB >= gridF - 20.0 * std::log10(gmin) - 1e-6, "at its own frequency the field is at least grid + loss there: the phase term is IN it, not dropped");
        }
        //  (n) IT AGREES WITH THIS FILE'S OWN INDEPENDENT SCAN at the worst frequency -- a 4000-point EVEN position
        //      scan of one extremum against the raw sample and every phase at its effective instant (the
        //      arg H / w model, the effective-instant one), written here separately.  A coarser grid can only read a minimum from
        //      above (a smaller under-read); the field must never be smaller than it and must reach it closely.
        {
            const auto ph = truePeakPolyphase(); const int taps = (int)ph[0].size(), half = taps / 2, P = (int)ph.size();
            const double w = 2 * M_PI * fStar; const int m = (int)std::lround(1.0 / (2.0 * fStar));   // fs/4 = fs/(2m), m = 2: ONE offset
            std::vector<double> mag(P + 1, 1.0), tau(P + 1, 0.0);
            for (int q = 0; q < P; ++q) { double re = 0, im = 0;
                for (int tt = 0; tt < taps; ++tt) { const double mm = tt - half + 1; re += ph[q][tt] * std::cos(w * mm); im += ph[q][tt] * std::sin(w * mm); }
                mag[q + 1] = std::sqrt(re * re + im * im); tau[q + 1] = std::atan2(im, re) / w; }
            double worstRead = 1e300;
            for (int k = 0; k < 4000; ++k) { const double p = k / 4000.0; double best = 0.0;
                for (int n = -(m / 2 + 2); n <= m / 2 + 2; ++n) for (int g = 0; g <= P; ++g) best = std::max(best, mag[g] * std::fabs(std::cos(w * (n + tau[g] - p))));
                worstRead = std::min(worstRead, best); }
            const double scan = -20.0 * std::log10(worstRead);
            check(scan <= sineB + 1e-9, "the field is never smaller than the coarse independent scan (it is a maximum of minima)");
            check(sineB - scan < 1e-6, "and the coarse scan reaches it within 1e-6 dB (the worst alignment is on a 4000-point grid for this table)");
        }
        //  (o) THE MODEL IS THE METER.  Long tones through the INTERIOR reading, 32 alignments in steps of 1/32 of a
        //      sample, at FOUR frequencies whose extrema visit one, one, two and four grid offsets: at the worst
        //      frequency (fs/4) the meter's worst alignment reaches the FIELD within 5e-4 dB; at fs/6, fs/3 and 2fs/5
        //      it reaches each frequency's own W(a, b) within 5e-4 dB -- the effective-instant model reproduces the
        //      meter in four places, two of them tones the grid DOES get a second look at (the rule: execute the thing
        //      the assertion is about; this beat's first reference model skipped every odd denominator and the meter
        //      drive caught it).  Then at FOUR other in-band frequencies -- the passband edge, 5fs/12, 3fs/8, 3fs/10
        //      -- the meter never reads lower than the truth minus the field.
        {
            auto worstInterior = [&](double f) {
                double worst = 1e9;
                for (int o = 0; o < 32; ++o) {
                    AudioBuffer sn = makeBuffer(1, sr, 4096);
                    for (size_t i = 0; i < 4096; ++i) setSample(sn, i, 0, std::sin(2 * M_PI * f * ((double)i + o / 32.0)));
                    double interior = 0.0;
                    const bool valid = truePeakDbInterior(sn, 0, interior);
                    check(valid, "the interior reading is valid on a 4096-sample buffer");
                    worst = std::min(worst, interior);                   // truth is 0 dBFS
                }
                return worst;
            };
            const double atStar = worstInterior(fStar);
            check(std::fabs(atStar + sineB) < 5e-4, "at the worst frequency the meter's worst alignment reaches the FIELD within 5e-4 dB: the model is the meter");
            struct AB { int a, b; const char* what; };
            const AB members[] = { {1, 6, "fs/6 (one offset)"}, {1, 3, "fs/3 (two offsets)"}, {2, 5, "2fs/5 (four offsets)"} };
            for (const AB& m : members) {
                const double wab = truePeakSineWorstDbOf(truePeakPolyphase(), m.a, m.b), at = worstInterior((double)m.a / m.b);
                check(std::fabs(at + wab) < 5e-4, std::string("at ") + m.what + " the meter reaches that frequency's own W(a, b) within 5e-4 dB: the model is the meter there too");
                check(wab < sineB, std::string(m.what) + " reads better than the worst frequency for this table (the maximum is the field)");
            }
            const double others[] = { kTruePeakPassbandEdge, 5.0 / 12.0, 3.0 / 8.0, 0.3 };
            for (double f : others) {
                const double wf = worstInterior(f);
                check(wf >= -sineB - 1e-4, "a steady tone at an in-band frequency off the family never reads lower than the truth minus the field (the edge among them)");
            }
        }
        //  (p) A PLANTED TABLE MOVES IT, AND A PLANTED INSTANT MOVES IT.  x0.99 on every coefficient: exactly
        //      20 log10(1/0.99) more (the raw sample does not scale, but it is not the winning instant at the worst
        //      alignment).  Phase 3 made a COPY of phase 2: two phases on one instant, the gap to phase 4 doubled,
        //      magnitudes those of a shipped phase -- the number must RISE, which a computation that read magnitudes
        //      and ignored instants would not show.  (A whole-sample delay of a phase is invisible to a 1-periodic
        //      lattice and was the first draft of this control; the reference model keeps that reading.)
        {
            auto planted = truePeakPolyphase();
            for (auto& hp : planted) for (auto& coef : hp) coef *= 0.99;
            const double moved = truePeakSineLowBoundOf(planted, kTruePeakPassbandEdge).db;
            check(std::fabs(moved - (sineB - 20.0 * std::log10(0.99))) < 1e-6, "a planted table (every coefficient x 0.99) MOVES the sine low bound by exactly 20 log10(1/0.99)");
            auto instant = truePeakPolyphase(); instant[3] = instant[2];
            const double risen = truePeakSineLowBoundOf(instant, kTruePeakPassbandEdge).db;
            check(risen - sineB > 0.05, "a planted INSTANT error (phase 3 a copy of phase 2) RAISES the bound by more than 0.05 dB: the effective instants are in the computation");
        }
        //  (q) THE EDGE IS REAL.  Naming fs/2 as the passband edge admits m = 1 (a tone at fs/2), where the phases
        //      have rolled off by design and a peak midway between samples is not seen at all -- a number in the
        //      hundreds of dB, not a metering error; the constant describes the table, as (l) already asserts.
        {
            const TruePeakSineLowBound half = truePeakSineLowBoundOf(truePeakPolyphase(), 0.5);
            check(std::fabs(half.frequency - 0.5) < 1e-12 && half.db > 10.0, "naming fs/2 as the edge admits m = 1 and reads a huge 'low side' (the roll-off), so the constant describes the table");
        }
    }

    // ---- 4. K-weighting: ~flat at 1 kHz, attenuates lows, +6 dB doubling ----
    {
        AudioBuffer k1 = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(k1, i, 0, 0.5 * std::sin(2 * M_PI * 1000 * i / sr));
        ChannelMetrics m1k = analyzeChannel(k1, 0);
        // Near 1 kHz the K-curve is close to 0 dB, so K-level ~= RMS - 0.691 within ~1 dB.
        check(near(m1k.kLevelLkfs, m1k.rmsDb - 0.691, 1.2), "K-level ~ RMS at 1 kHz");

        AudioBuffer klo = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(klo, i, 0, 0.5 * std::sin(2 * M_PI * 30 * i / sr));
        ChannelMetrics mlo = analyzeChannel(klo, 0);
        check(mlo.kLevelLkfs < mlo.rmsDb - 8.0, "K-weighting attenuates 30 Hz strongly (>8 dB)");

        AudioBuffer k2 = makeBuffer(1, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(k2, i, 0, 1.0 * std::sin(2 * M_PI * 1000 * i / sr));
        ChannelMetrics m2 = analyzeChannel(k2, 0);
        check(near(m2.kLevelLkfs - m1k.kLevelLkfs, 6.02, 0.05), "doubling amplitude: +6 dB K-level");
    }

    // ---- 4b. The K-weighting biquads must match BS.1770-4 Table 1 ----
    // Section 4 above gates the K-curve against FLATNESS at 1 kHz to +/- 1.2 dB. That is 28x
    // too loose to see a 0.043 dB error, which is how the RLB numerator sat non-conformant
    // through every prior release. A conformance check must gate against the TABLE, not
    // against a property the wrong filter also has.
    {
        // BS.1770-4 Table 1, the tabulated 48 kHz coefficients, quoted to the 15 significant
        // digits the table prints. The table's own truncation floor is ~1e-12, so that is the
        // tolerance -- stating the source's precision beside the tolerance, because a
        // threshold set below the data's precision reports a difference that is not there.
        const double kT1Tol = 1e-12;
        const Biquad sh = k1ShelfBiquad(48000.0);
        check(near(sh.b0,  1.53512485958697, kT1Tol), "Table 1: shelf b0");
        check(near(sh.b1, -2.69169618940638, kT1Tol), "Table 1: shelf b1");
        check(near(sh.b2,  1.19839281085285, kT1Tol), "Table 1: shelf b2");
        check(near(sh.a1, -1.69065929318241, kT1Tol), "Table 1: shelf a1");
        check(near(sh.a2,  0.73248077421585, kT1Tol), "Table 1: shelf a2");

        const Biquad hp = k2HighpassBiquad(48000.0);
        check(near(hp.b0,  1.0,              kT1Tol), "Table 1: RLB b0 is 1 (NOT 1/a0)");
        check(near(hp.b1, -2.0,              kT1Tol), "Table 1: RLB b1 is -2 (NOT -2/a0)");
        check(near(hp.b2,  1.0,              kT1Tol), "Table 1: RLB b2 is 1 (NOT 1/a0)");
        check(near(hp.a1, -1.99004745483398, kT1Tol), "Table 1: RLB a1");
        check(near(hp.a2,  0.99007225036621, kT1Tol), "Table 1: RLB a2");

        // The mechanism, asserted as a relationship rather than as a literal: the tabulated
        // RLB's gain at z = -1 is exactly a0, because the numerator is not divided by it.
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(M_PI * f0 / 48000.0);
        const double a0 = 1.0 + K / Q + K * K;
        const double den = 1.0 - hp.a1 + hp.a2;
        check(near((hp.b0 - hp.b1 + hp.b2) / den, a0, kT1Tol), "RLB passband gain is a0");

        // NEGATIVE CONTROL. Rebuild the earlier form and prove this check REJECTS it.
        // A conformance check that cannot fail the known-bad filter is not a check --
        // it can only agree, and a control that can only agree is not a control.
        const double oldGain = ((1.0 / a0) - (-2.0 / a0) + (1.0 / a0)) / den;
        check(near(oldGain, 1.0, kT1Tol), "control: the OLD normalised RLB gain was exactly 1.0");
        check(!near(1.0 / a0, 1.0, kT1Tol), "control: this check REJECTS the pre-fix numerator");
        check(near(20.0 * std::log10(a0 / oldGain), 0.04327714626081623, 1e-9),
              "control: the correction was worth 0.043277 LU at 48 kHz");

        // And the offset at every rate the product supports, so a regression names its own
        // magnitude. Values recorded earlier and re-derived when the fix landed.
        const struct { double fs; double lu; const char* name; } kRates[] = {
            {44100.0, 0.047099, "44.1 kHz"}, {48000.0, 0.043277, "48 kHz"},
            {88200.0, 0.023566, "88.2 kHz"}, {96000.0, 0.021652, "96 kHz"},
        };
        for (const auto& r : kRates) {
            const Biquad q = k2HighpassBiquad(r.fs);
            const double g = (q.b0 - q.b1 + q.b2) / (1.0 - q.a1 + q.a2);
            check(near(20.0 * std::log10(g), r.lu, 5e-7),
                  std::string("RLB passband gain in LU at ") + r.name);
        }
    }

    // ---- 5. inter-channel correlation ----
    {
        AudioBuffer b = makeBuffer(3, sr, N);
        std::mt19937 rng(12345);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        for (size_t i = 0; i < N; ++i) {
            double x = std::sin(2 * M_PI * 500 * i / sr);
            setSample(b, i, 0, x);         // ch0
            setSample(b, i, 1, -x);        // ch1 = inverted ch0
            setSample(b, i, 2, u(rng));    // ch2 = independent noise
        }
        check(near(interChannelCorrelation(b, 0, 0), 1.0, 1e-6), "corr(ch,ch) = +1");
        check(near(interChannelCorrelation(b, 0, 1), -1.0, 1e-6), "corr(x,-x) = -1 (out of phase)");
        check(std::fabs(interChannelCorrelation(b, 0, 2)) < 0.1, "corr(signal, indep noise) ~ 0");
    }

    // ---- 6. ambisonic DoA + diffuseness + rV for a single encoded source (FOA) ----
    {
        struct TC { double az, el; };
        TC cases[] = {{0, 0}, {90, 0}, {-90, 0}, {45, 30}, {0, 90}, {135, -20}};
        for (const TC& c : cases) {
            AudioBuffer b = encodeSource(c.az, c.el, 1, sr, N);
            AmbiField f = analyzeAmbisonic(b, 1, false);
            check(f.valid, "FOA field valid");
            if (c.el < 89.0)  // azimuth is degenerate straight overhead
                check(angDiff(f.doaAzDeg, c.az) < 2.0,
                      "DoA azimuth ~ source az (" + std::to_string((int)c.az) + ")");
            check(angDiff(f.doaElDeg, c.el) < 2.0,
                  "DoA elevation ~ source el (" + std::to_string((int)c.el) + ")");
            check(f.diffuseness < 0.02, "single plane wave: diffuseness ~ 0");
            check(near(f.rVmag, 1.0, 0.02), "single plane wave: |rV| ~ 1");
            check(f.rEmag > 0.4 && f.rEmag <= 1.0, "|rE| in (0,1] for a point source");
        }
    }

    // ---- 7. fully diffuse field: diffuseness ~ 1, |rV| ~ 0 ----
    {
        AudioBuffer b = makeBuffer(4, sr, N);
        std::mt19937 rng(777);
        std::normal_distribution<double> g(0.0, 0.3);
        for (size_t i = 0; i < N; ++i)
            for (int k = 0; k < 4; ++k) setSample(b, i, k, g(rng));  // independent per ACN
        AmbiField f = analyzeAmbisonic(b, 1, false);
        check(f.diffuseness > 0.8, "decorrelated B-format: diffuseness > 0.8");
        check(f.rVmag < 0.2, "decorrelated B-format: |rV| < 0.2");
    }

    // ---- 8. rE sharpens with ambisonic order (order awareness) ----
    {
        AudioBuffer o1 = encodeSource(60, 10, 1, sr, N);
        AudioBuffer o3 = encodeSource(60, 10, 3, sr, N);
        AmbiField f1 = analyzeAmbisonic(o1, 1, false);
        AmbiField f3 = analyzeAmbisonic(o3, 3, false);
        check(f3.rEmag > f1.rEmag + 0.05, "|rE| increases with order (sharper localization)");
        check(angDiff(f3.rEazDeg, 60) < 3.0, "order-3 rE azimuth ~ source az");
        check(angDiff(f3.rEelDeg, 10) < 3.0, "order-3 rE elevation ~ source el");
        check(f3.order == 3 && f3.acnUsed == 16, "order-3 uses 16 ACN channels");
    }

    // ---- 9. order inference + guards ----
    {
        check(ambiOrderFromChannels(4) == 1, "4 ch -> order 1");
        check(ambiOrderFromChannels(9) == 2, "9 ch -> order 2");
        check(ambiOrderFromChannels(16) == 3, "16 ch -> order 3");
        check(ambiOrderFromChannels(2) == 0, "2 ch -> order 0 (not ambisonic)");
        AudioBuffer stereo = makeBuffer(2, sr, 100);
        AmbiField f = analyzeAmbisonic(stereo, 0, false);
        check(!f.valid, "stereo buffer -> invalid ambisonic field (fail closed)");
    }

    // ================================ Batch M2 additions ================================

    // ---- 10. BS.1770-4 gated integrated loudness ----
    {
        // (a) steady sine: gating changes nothing, activity ~ 1.
        AudioBuffer steady = makeBuffer(2, sr, (size_t)(5.0 * sr));
        for (size_t i = 0; i < steady.frames; ++i) {
            double s = 0.5 * std::sin(2 * M_PI * 997 * i / sr);
            setSample(steady, i, 0, s); setSample(steady, i, 1, s);
        }
        GatedLoudness gs = gatedLoudness(steady);
        check(gs.valid, "gated loudness valid on steady sine");
        check(near(gs.integratedLufs, gs.ungatedLufs, 0.3), "steady sine: gated ~= ungated");
        check(gs.activityFraction > 0.95, "steady sine: activity ~ 1");

        // (b) 1 s burst + 9 s silence: the gates drop the silence -> gated ~= burst-only level,
        //     ungated is ~10 dB lower (energy spread over 10x the time), activity ~ 0.1.
        AudioBuffer burst = makeBuffer(1, sr, (size_t)(10.0 * sr));
        for (size_t i = 0; i < (size_t)sr; ++i)
            setSample(burst, i, 0, 0.5 * std::sin(2 * M_PI * 997 * i / sr));
        AudioBuffer only = makeBuffer(1, sr, (size_t)sr);
        for (size_t i = 0; i < only.frames; ++i)
            setSample(only, i, 0, 0.5 * std::sin(2 * M_PI * 997 * i / sr));
        GatedLoudness gb = gatedLoudness(burst);
        GatedLoudness go = gatedLoudness(only);
        check(near(gb.integratedLufs, go.ungatedLufs, 1.0), "burst: gated ~= level of the burst itself");
        check(gb.ungatedLufs < gb.integratedLufs - 8.0, "burst: ungated ~10 dB below gated");
        check(gb.activityFraction > 0.05 && gb.activityFraction < 0.2, "burst: activity ~ 0.1");

        // (c) channel weights: zero-weighting one of two identical channels drops 3.01 dB; a 1.41
        //     surround weight adds ~1.5 dB.
        GatedLoudness gBoth = gatedLoudness(steady, {1.0, 1.0});
        GatedLoudness gOne  = gatedLoudness(steady, {1.0, 0.0});
        check(near(gBoth.integratedLufs - gOne.integratedLufs, 3.01, 0.1),
              "zero-weight (LFE-style) exclusion drops one of two equal channels by 3 dB");
        GatedLoudness gSurr = gatedLoudness(steady, {1.0, 1.41});
        check(gSurr.integratedLufs > gBoth.integratedLufs + 0.5,
              "1.41 surround weight raises the weighted loudness");

        // (d) sub-block buffer falls back to ungated.
        AudioBuffer tiny = makeBuffer(1, sr, (size_t)(0.2 * sr));
        for (size_t i = 0; i < tiny.frames; ++i)
            setSample(tiny, i, 0, 0.5 * std::sin(2 * M_PI * 997 * i / sr));
        GatedLoudness gt = gatedLoudness(tiny);
        check(gt.valid && near(gt.integratedLufs, gt.ungatedLufs, 1e-9),
              "sub-400ms buffer: integrated falls back to ungated");
    }

    // ---- 11. momentary / short-term timeline + LRA ----
    {
        // 12 s mono: 6 s at 0.5, then 6 s at 0.05 (-20 dB).
        AudioBuffer b = makeBuffer(1, sr, (size_t)(12.0 * sr));
        for (size_t i = 0; i < b.frames; ++i) {
            double amp = (i < (size_t)(6.0 * sr)) ? 0.5 : 0.05;
            setSample(b, i, 0, amp * std::sin(2 * M_PI * 997 * i / sr));
        }
        LoudnessTimeline tl = loudnessTimeline(b, {}, 0.1);
        check(tl.valid, "timeline valid");
        check(tl.times.size() == tl.momentary.size() && tl.times.size() == tl.shortTerm.size(),
              "timeline arrays aligned");
        check(tl.times.size() > 100, "12 s at 100 ms hop yields >100 points");
        check(tl.maxMomentaryTime < 6.5, "max momentary sits in the loud half");
        check(near(tl.maxMomentaryLufs, tl.momentary.front(), 1.0),
              "loud-half momentary is the max");
        // last momentary window is fully in the quiet half: ~20 dB below the max.
        check(near(tl.maxMomentaryLufs - tl.momentary.back(), 20.0, 1.0),
              "quiet-half momentary ~20 dB below max");
        check(tl.lraLu > 10.0, "LRA sees the 20 dB loud/quiet split (>10 LU)");
        check(tl.maxShortTermLufs <= tl.maxMomentaryLufs + 0.5,
              "max short-term <= max momentary (longer window smooths)");
    }

    // ---- 12. downmix matrix ----
    {
        // L == R sine -> mono (0.5/0.5) preserves the waveform exactly.
        AudioBuffer st = makeBuffer(2, sr, N);
        for (size_t i = 0; i < N; ++i) {
            double s = 0.5 * std::sin(2 * M_PI * 500 * i / sr);
            setSample(st, i, 0, s); setSample(st, i, 1, s);
        }
        AudioBuffer mono = applyDownmixMatrix(st, {{0.5, 0.5}});
        check(mono.channels == 1 && mono.frames == st.frames, "downmix shape (2 -> 1)");
        ChannelMetrics mm = analyzeChannel(mono, 0);
        ChannelMetrics ml = analyzeChannel(st, 0);
        check(near(mm.peakDb, ml.peakDb, 0.05), "correlated fold: mono peak == source peak");

        // L == -R -> mono cancels to silence.
        AudioBuffer anti = makeBuffer(2, sr, N);
        for (size_t i = 0; i < N; ++i) {
            double s = 0.5 * std::sin(2 * M_PI * 500 * i / sr);
            setSample(anti, i, 0, s); setSample(anti, i, 1, -s);
        }
        AudioBuffer monoA = applyDownmixMatrix(anti, {{0.5, 0.5}});
        check(analyzeChannel(monoA, 0).peakDb <= -100.0, "anti-phase fold cancels to silence");

        // 5.1 C-only content folds to both stereo outs at -3 dB, fully correlated.
        AudioBuffer c51 = makeBuffer(6, sr, N);
        for (size_t i = 0; i < N; ++i) setSample(c51, i, 2, 0.5 * std::sin(2 * M_PI * 700 * i / sr));
        const double q = 1.0 / std::sqrt(2.0);
        AudioBuffer fold = applyDownmixMatrix(c51, {{1, 0, q, 0, q, 0}, {0, 1, q, 0, 0, q}});
        check(fold.channels == 2, "5.1 -> stereo shape");
        ChannelMetrics fl = analyzeChannel(fold, 0);
        check(near(fl.peakDb, linToDb(0.5 * q), 0.05), "centre folds at -3 dB into each side");
        check(near(interChannelCorrelation(fold, 0, 1), 1.0, 1e-6), "centre-only fold: Lo/Ro corr = +1");
    }

    // ---- 13. windowed spatial-field timeline (moving source) ----
    {
        // 2 s FOA: source at az=90 (left) for the first second, az=0 (front) for the second.
        AudioBuffer a = encodeSource(90, 0, 1, sr, (size_t)sr);
        AudioBuffer bfr = encodeSource(0, 0, 1, sr, (size_t)sr);
        AudioBuffer seq = makeBuffer(4, sr, (size_t)(2.0 * sr));
        for (size_t i = 0; i < (size_t)sr; ++i)
            for (int k = 0; k < 4; ++k) {
                setSample(seq, i, k, a.at(i, k));
                setSample(seq, i + (size_t)sr, k, bfr.at(i, k));
            }
        std::vector<AmbiFieldWindow> ws = analyzeAmbisonicTimeline(seq, 0.5, 0.5, 1, false);
        check(ws.size() == 4, "2 s / 0.5 s windows -> 4 windows");
        check(ws.front().field.valid && ws.back().field.valid, "timeline windows valid");
        check(angDiff(ws.front().field.doaAzDeg, 90) < 3.0, "window 0 DoA ~ az 90 (left)");
        check(angDiff(ws.back().field.doaAzDeg, 0) < 3.0, "window 3 DoA ~ az 0 (front)");
        check(near(ws[1].timeSec, 0.5, 1e-9), "window 1 starts at 0.5 s");

        // Shorter than one window -> a single whole-buffer window.
        std::vector<AmbiFieldWindow> one = analyzeAmbisonicTimeline(a, 5.0, 1.0, 1, false);
        check(one.size() == 1 && one[0].field.valid, "short buffer -> single fallback window");
        check(angDiff(one[0].field.doaAzDeg, 90) < 2.0, "fallback window DoA correct");
    }

    // ================================ Batch M3 additions ================================

    // ---- 14. decode coverage (virtual-speaker decode + coverage statistics) ----
    {
        const size_t frames = (size_t)(2.0 * sr);
        std::vector<Dir> sph = fibonacciSphere(120);
        check(sph.size() == 120, "fibonacci sphere returns the requested speaker count");

        // (a) single plane wave (FOA, ambisonic az=90 = LEFT): energy concentrates -> NOT uniform,
        //     dead zones near the antipode, dominant decode direction ~ the source, left-weighted.
        AudioBuffer pw = encodeSource(90, 0, 1, sr, frames, 440.0, 0.5);
        std::vector<double> Ep = decodeSpeakerEnergies(pw, 0, pw.frames, sph, 1, false);
        CoverageStats cp = computeCoverageStats(sph, Ep, 0.1);
        check(cp.valid, "coverage valid on a plane wave");
        check(cp.uniformity < 0.95, "plane wave: coverage below perfectly uniform");
        check(cp.cv > 0.5, "plane wave: high coefficient of variation (energy uneven across directions)");
        check(cp.deadZones > 0, "plane wave: has dead zones (energy-starved directions)");
        check(cp.concentrationTop > 0.2, "plane wave: energy concentrates in the loudest directions");
        check(angDiff(cp.dominantAzDeg, 90) < 20.0 && std::fabs(cp.dominantElDeg) < 20.0,
              "plane wave: dominant decode direction ~ the source (az 90)");
        check(cp.leftFrac > cp.rightFrac, "plane wave at az=90 (left): left hemisphere carries more");

        // (b) decorrelated equal-power field (distinct freq per ACN channel, integer cycles over the
        //     window => orthogonal): the addition theorem makes the decode direction-independent, so
        //     coverage is essentially perfectly uniform with no dead zones.
        AudioBuffer diff = makeBuffer(4, sr, frames);
        const double fch[4] = {307.0, 503.0, 701.0, 1103.0};
        for (size_t i = 0; i < frames; ++i)
            for (int k = 0; k < 4; ++k)
                setSample(diff, i, k, 0.4 * std::sin(2 * M_PI * fch[k] * (double)i / sr));
        std::vector<double> Ed = decodeSpeakerEnergies(diff, 0, diff.frames, sph, 1, false);
        CoverageStats cd = computeCoverageStats(sph, Ed, 0.1);
        check(cd.valid, "coverage valid on a decorrelated field");
        check(cd.uniformity > cp.uniformity + 0.05,
              "decorrelated field is MORE uniform than a plane wave");
        check(cd.uniformity > 0.99, "decorrelated equal-power field: essentially perfect coverage");
        check(cd.cv < 0.1, "decorrelated field: near-zero coefficient of variation");
        check(cd.deadZones == 0, "decorrelated field: no dead zones");

        // (c) silent scene -> invalid coverage (no energy to distribute).
        AudioBuffer sil = makeBuffer(4, sr, (size_t)sr);
        CoverageStats cs =
            computeCoverageStats(sph, decodeSpeakerEnergies(sil, 0, sil.frames, sph, 1, false), 0.1);
        check(!cs.valid, "silent scene: coverage invalid (no energy)");

        // (d) front source -> front hemisphere dominates, dominant az ~ 0.
        AudioBuffer front = encodeSource(0, 0, 1, sr, frames, 440.0, 0.5);
        std::vector<double> Ef = decodeSpeakerEnergies(front, 0, front.frames, sph, 1, false);
        CoverageStats cf = computeCoverageStats(sph, Ef, 0.1);
        check(cf.frontFrac > cf.backFrac, "front source: front hemisphere carries more energy");
        check(std::fabs(cf.dominantAzDeg) < 20.0, "front source: dominant az ~ 0");

        // (e) N3D flag: converting an N3D-scaled scene back to SN3D restores the SN3D coverage. Scale
        //     each channel by sqrt(2l+1) (SN3D->N3D), decode with isN3D=true, expect the SN3D result.
        AudioBuffer n3d = pw;
        for (size_t i = 0; i < n3d.frames; ++i)
            for (int k = 0; k < n3d.channels; ++k) {
                int l = (int)std::floor(std::sqrt((double)k) + 1e-9);
                n3d.samples[i * (size_t)n3d.channels + k] *= (float)std::sqrt(2.0 * l + 1.0);
            }
        CoverageStats cn =
            computeCoverageStats(sph, decodeSpeakerEnergies(n3d, 0, n3d.frames, sph, 1, true), 0.1);
        check(angDiff(cn.dominantAzDeg, cp.dominantAzDeg) < 1.0 &&
              near(cn.uniformity, cp.uniformity, 0.02),
              "N3D->SN3D conversion reproduces the SN3D coverage");
    }

    // ---- Direct-sample helpers (audio-accessor path): DC, clip, overview, silence scan ----------
    // These back analysis.read_samples / accessor_meter / detect_silence; the accessor lifecycle
    // itself is SDK-only and lives-verified separately (scripts/verify_accessors.py).
    {
        const double sr = 48000.0;
        // (a) DC offset: a constant-offset channel reports its mean; a zero-mean tone reports ~0.
        AudioBuffer dc = makeBuffer(2, sr, 1000);
        for (size_t i = 0; i < dc.frames; ++i) {
            setSample(dc, i, 0, 0.25);
            setSample(dc, i, 1, std::sin(2.0 * M_PI * 480.0 * (double)i / sr));  // 10 whole cycles/1000
        }
        check(near(channelDcOffset(dc, 0), 0.25, 1e-6), "channelDcOffset: constant channel == offset");
        check(near(channelDcOffset(dc, 1), 0.0, 1e-3), "channelDcOffset: zero-mean tone ~ 0");

        // (b) clip count: samples at/above the threshold are counted; below are not.
        AudioBuffer cl = makeBuffer(1, sr, 100);
        for (size_t i = 0; i < 10; ++i) setSample(cl, i, 0, 1.0);
        for (size_t i = 10; i < 20; ++i) setSample(cl, i, 0, -1.0);
        for (size_t i = 20; i < 100; ++i) setSample(cl, i, 0, 0.5);
        check(channelClipCount(cl, 0, 0.999) == 20, "channelClipCount: 20 full-scale samples counted");
        check(channelClipCount(cl, 0, 1.5) == 0, "channelClipCount: none above an unreachable threshold");

        // (c) overview: min/max per bucket; bucket count == requested; capped to frames; empty if none.
        AudioBuffer rp = makeBuffer(1, sr, 400);
        for (size_t i = 0; i < rp.frames; ++i)
            setSample(rp, i, 0, -1.0 + 2.0 * (double)i / (double)(rp.frames - 1));
        auto ov = channelOverview(rp, 0, 4);
        check(ov.size() == 4, "channelOverview: returns the requested bucket count");
        check(ov.front().min <= -0.99f, "channelOverview: first bucket reaches the ramp minimum");
        check(ov.back().max >= 0.99f, "channelOverview: last bucket reaches the ramp maximum");
        check(channelOverview(rp, 0, 100000).size() == rp.frames,
              "channelOverview: buckets capped to the frame count");
        check(channelOverview(makeBuffer(1, sr, 0), 0, 8).empty(),
              "channelOverview: empty on a zero-frame buffer");

        // (d) silence scan: 0.5 s silence + 1 s tone + 0.5 s silence -> leading + trailing regions.
        const size_t half = (size_t)(0.5 * sr), one = (size_t)(1.0 * sr);
        AudioBuffer sg = makeBuffer(1, sr, half + one + half);
        for (size_t i = 0; i < one; ++i)
            setSample(sg, half + i, 0, 0.5 * std::sin(2.0 * M_PI * 440.0 * (double)i / sr));
        auto regs = scanSilence(sg, -60.0, (size_t)(0.010 * sr), (size_t)(0.2 * sr));
        check(regs.size() == 2, "scanSilence: leading + trailing silence detected (tone kept)");
        if (regs.size() == 2) {
            check(regs[0].startFrame == 0 && near((double)regs[0].endFrame, (double)half, 0.05 * sr),
                  "scanSilence: leading region ~ [0, 0.5 s)");
            check(regs[1].endFrame == sg.frames &&
                  near((double)regs[1].startFrame, (double)(half + one), 0.05 * sr),
                  "scanSilence: trailing region ~ [1.5 s, end)");
        }
        // (e) all-silent -> one full-length region; all-loud -> none.
        AudioBuffer sil = makeBuffer(1, sr, one);
        auto rs = scanSilence(sil, -60.0, (size_t)(0.010 * sr), (size_t)(0.2 * sr));
        check(rs.size() == 1 && rs[0].startFrame == 0 && rs[0].endFrame == sil.frames,
              "scanSilence: fully-silent buffer -> one full-length region");
        AudioBuffer loud = makeBuffer(1, sr, one);
        for (size_t i = 0; i < loud.frames; ++i)
            setSample(loud, i, 0, 0.5 * std::sin(2.0 * M_PI * 440.0 * (double)i / sr));
        check(scanSilence(loud, -60.0, (size_t)(0.010 * sr), (size_t)(0.2 * sr)).empty(),
              "scanSilence: fully-loud buffer -> no silent regions");
    }

    if (g_failures == 0) std::fprintf(stderr, "\nALL METER DSP TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d METER DSP TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
