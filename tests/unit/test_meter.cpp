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
