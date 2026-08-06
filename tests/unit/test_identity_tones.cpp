// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_identity_tones.cpp — pure unit test for the B4 channel-identity QC core
// (src/identity_tones.h): the corpus tone plan (313 + 139*k Hz, LFE 40 Hz), the Goertzel
// detector, and the fixed verdict vocabulary. Doc 135; preregistration-135.md pins the
// expectations E-135.1 .. E-135.4 and fixes the verdict names BEFORE any output existed.
//
// The round trip is deliberately through the SHIPPED writer (loomb::writeWavPcm) and the
// SHIPPED reader (meter::readWavFile), because that is the exact path both verbs use: a
// WAV on disk, or an accessor read, reduced to one meter::AudioBuffer. The fault cases are
// CONSTRUCTED in the buffer, so each verdict has a positive control, and E-135.4 checks the
// gate can actually fail — a detector that never says no is not a detector.
//
// No REAPER, no SDK. Every number is deterministic.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "identity_tones.h"
#include "loom_manifest.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Windows CI: /tmp does not exist there (doc 97/99 class; the same fix test_meter.cpp carries).
static std::string tmpPath(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::path(".");
    return (dir / name).string();
}

// Write the shipped writer's bytes to disk and read them back with the shipped reader.
static bool roundTrip(const std::vector<std::vector<float>>& chans, size_t frames, int rate,
                      int bits, const char* name, meter::AudioBuffer& out, std::string& err) {
    const std::string bytes = loomb::writeWavPcm(chans, frames, rate, bits);
    const std::string path = tmpPath(name);
    {
        std::ofstream f(path, std::ios::binary);
        if (!f) { err = "cannot open " + path; return false; }
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    const bool ok = meter::readWavFile(path, out, err);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ok;
}

// De-interleave a buffer so a test can corrupt it channel-wise, then re-interleave.
static std::vector<std::vector<float>> split(const meter::AudioBuffer& b) {
    std::vector<std::vector<float>> ch((size_t)b.channels, std::vector<float>(b.frames, 0.0f));
    for (size_t f = 0; f < b.frames; ++f)
        for (int c = 0; c < b.channels; ++c) ch[(size_t)c][f] = b.at(f, c);
    return ch;
}
static meter::AudioBuffer join(const std::vector<std::vector<float>>& ch, double sr) {
    meter::AudioBuffer b;
    b.channels = (int)ch.size();
    b.sampleRate = sr;
    b.frames = ch.empty() ? 0 : ch[0].size();
    b.samples.assign(b.frames * (size_t)b.channels, 0.0f);
    for (size_t f = 0; f < b.frames; ++f)
        for (int c = 0; c < b.channels; ++c) b.samples[f * (size_t)b.channels + (size_t)c] = ch[(size_t)c][f];
    return b;
}

static const char* vn(idtone::Verdict v) { return idtone::verdictName(v); }

int main() {
    namespace it = idtone;

    // ==== E-135.1 — round-trip identity at five widths, worst margin >= 60 dB ==================
    // 2 = stereo, 6 = 5.1, 12 = 7.1.4, 16 = 9.1.6, 24 = 22.2. Doc 116 measured 12/6/2; 16 and 24
    // are new here, and 24 is the widest bed the product declares.
    {
        for (int n : {2, 6, 12, 16, 24}) {
            const it::Plan plan = it::makePlan(n, /*lfeIdx*/ {}, /*labels*/ {}, 48000, 1.0);
            check(plan.frames() == 48000, "width " + std::to_string(n) + ": 1 s @48k = 48000 frames");
            const auto chans = it::renderPlan(plan);

            meter::AudioBuffer buf;
            std::string err;
            const std::string nm = "insg_b4_" + std::to_string(n) + ".wav";
            const bool rt = roundTrip(chans, plan.frames(), plan.rate, 24, nm.c_str(), buf, err);
            check(rt && buf.channels == n && (int)buf.sampleRate == 48000,
                  "width " + std::to_string(n) + ": WAV round-trips 48k/24-bit through the "
                  "shipped writer + reader" + (rt ? "" : (" [" + err + "]")));
            if (!rt) continue;

            const it::RoutingReport r = it::detectRouting(buf, plan);
            check(r.error.empty(), "width " + std::to_string(n) + ": no structural refusal");
            check(r.ok, "width " + std::to_string(n) + ": ok=true (identity on every channel)");
            check(r.identityCount == n,
                  "width " + std::to_string(n) + ": all " + std::to_string(n) + " identity");
            check(r.worstMarginDb >= 60.0,
                  "width " + std::to_string(n) + ": worst off-diagonal margin >= 60 dB (got " +
                      std::to_string((int)r.worstMarginDb) + " dB)");
            check(r.findings.empty(), "width " + std::to_string(n) + ": zero findings when clean");

            // the detected map is the identity permutation
            bool ident = true;
            const auto m = it::detectedMap(r);
            for (int k = 0; k < n; ++k) if (m[(size_t)k] != k) ident = false;
            check(ident, "width " + std::to_string(n) + ": detected map == identity permutation");

            // positive control for the coverage guard: a pure plan tone covers ~all of its RMS.
            double worstCov = 1e9;
            for (const auto& c : r.perChannel) worstCov = std::min(worstCov, c.planCoverage);
            check(worstCov > 0.98,
                  "width " + std::to_string(n) + ": planCoverage ~1.0 on every clean channel (worst " +
                      std::to_string(worstCov) + ")");
        }
    }

    // ==== E-135.2 — the LFE exception, WITH its negative control ==============================
    // 5.1: LFE is channel 3. With the rule on, channel 3 carries 40 Hz and still reads identity.
    // With the rule OFF the same *buffer* is judged against a plan that expects 313+139*3 there,
    // so channel 3 must misclassify — proving the rule is doing work and is not inert.
    {
        const std::vector<int> lfe{3};
        const std::vector<std::string> labels{"L", "R", "C", "LFE", "Ls", "Rs"};
        const it::Plan planOn = it::makePlan(6, lfe, labels, 48000, 1.0);
        check(std::abs(planOn.slots[3].hz - it::kLfeHz) < 1e-9 && planOn.slots[3].lfe,
              "LFE rule on: slot 3 is 40 Hz and flagged lfe");
        check(std::abs(planOn.slots[4].hz - it::toneHz(4)) < 1e-9,
              "LFE rule on: non-LFE slots keep 313+139*k (slot 4 = 869 Hz)");

        const auto chans = it::renderPlan(planOn);
        meter::AudioBuffer buf;
        std::string err;
        const bool rt = roundTrip(chans, planOn.frames(), planOn.rate, 24, "insg_b4_lfe.wav", buf, err);
        check(rt, std::string("LFE: WAV round-trips") + (rt ? "" : (" [" + err + "]")));
        if (rt) {
            const it::RoutingReport on = it::detectRouting(buf, planOn);
            check(on.ok, "LFE rule on: identity on all 6 channels including the 40 Hz LFE");
            check(on.perChannel[3].verdict == it::Verdict::Identity &&
                      std::abs(on.perChannel[3].expectHz - 40.0) < 1e-9,
                  "LFE rule on: channel 3 reads identity at 40 Hz");
            check(on.perChannel[3].label == "LFE", "LFE: labels are echoed into the report");

            // NEGATIVE CONTROL — same bytes, rule disabled in the PLAN.
            const it::Plan planOff = it::makePlan(6, lfe, labels, 48000, 1.0,
                                                  it::kDefaultLevelDb, /*lfeRule*/ false);
            check(std::abs(planOff.slots[3].hz - it::toneHz(3)) < 1e-9 && !planOff.slots[3].lfe,
                  "LFE negative control: with the rule off slot 3 expects 730 Hz");
            const it::RoutingReport off = it::detectRouting(buf, planOff);
            check(!off.ok, "LFE negative control: the same buffer FAILS against the no-LFE plan");
            check(off.perChannel[3].verdict != it::Verdict::Identity,
                  std::string("LFE negative control: channel 3 misclassifies (") +
                      vn(off.perChannel[3].verdict) + ")");
            // The channel's real content (40 Hz) is not in the no-LFE plan AT ALL, so the arg-max
            // over the plan's tones is noise. The coverage guard must say exactly that rather than
            // guess a swap or a duplicate from a near-zero winner. (Doc 135's post-hoc refinement,
            // discovered by this control.)
            check(off.perChannel[3].verdict == it::Verdict::Dropped,
                  std::string("LFE negative control: it reports 'dropped', not a guessed swap/dup "
                              "(got ") + vn(off.perChannel[3].verdict) + ")");
            check(off.perChannel[3].detectedSlot == -1 &&
                      off.perChannel[3].planCoverage < it::kPlanCoverageFloor,
                  "LFE negative control: no slot is claimed and planCoverage is below the floor");
            check(off.identityCount == 5,
                  "LFE negative control: the other five channels are unaffected");
        }
    }

    // ==== E-135.3 / E-135.4 — the four fault classes, each constructed ========================
    {
        const it::Plan plan = it::makePlan(6, /*lfeIdx*/ {}, /*labels*/ {}, 48000, 1.0);
        meter::AudioBuffer clean;
        std::string err;
        const bool rt = roundTrip(it::renderPlan(plan), plan.frames(), plan.rate, 24,
                                  "insg_b4_faults.wav", clean, err);
        check(rt, std::string("faults: base buffer round-trips") + (rt ? "" : (" [" + err + "]")));
        if (rt) {
            // --- gate negative control, half 1: clean buffer must pass with zero findings ---
            const it::RoutingReport base = it::detectRouting(clean, plan);
            check(base.ok && base.findings.empty(),
                  "E-135.4a: the clean buffer yields ok=true and zero findings");

            const double sr = clean.sampleRate;

            // --- swap channels 1 and 4 ---
            {
                auto ch = split(clean);
                std::swap(ch[1], ch[4]);
                const it::RoutingReport r = it::detectRouting(join(ch, sr), plan);
                check(!r.ok, "E-135.4b: swapped buffer yields ok=false");
                check(r.perChannel[1].verdict == it::Verdict::Swapped &&
                          r.perChannel[4].verdict == it::Verdict::Swapped,
                      std::string("swap: both channels report 'swapped' (got ") +
                          vn(r.perChannel[1].verdict) + "/" + vn(r.perChannel[4].verdict) + ")");
                check(r.perChannel[1].pairedWith == 4 && r.perChannel[4].pairedWith == 1,
                      "swap: each names the other as its partner");
                check(r.perChannel[0].verdict == it::Verdict::Identity &&
                          r.perChannel[5].verdict == it::Verdict::Identity,
                      "swap: untouched channels still read identity (no collateral)");
            }

            // --- zero channel 2 ---
            {
                auto ch = split(clean);
                std::fill(ch[2].begin(), ch[2].end(), 0.0f);
                const it::RoutingReport r = it::detectRouting(join(ch, sr), plan);
                check(!r.ok, "E-135.4c: dropped buffer yields ok=false");
                check(r.perChannel[2].verdict == it::Verdict::Silent,
                      std::string("drop: the zeroed channel reports 'silent' (got ") +
                          vn(r.perChannel[2].verdict) + ")");
                check(r.identityCount == 5, "drop: the other five are unaffected");
            }

            // --- copy channel 0 over channel 3 ---
            {
                auto ch = split(clean);
                ch[3] = ch[0];
                const it::RoutingReport r = it::detectRouting(join(ch, sr), plan);
                check(!r.ok, "E-135.4d: duplicated buffer yields ok=false");
                check(r.perChannel[3].verdict == it::Verdict::Duplicated,
                      std::string("dup: the overwritten channel reports 'duplicated' (got ") +
                          vn(r.perChannel[3].verdict) + ")");
                check(r.perChannel[3].pairedWith == 0,
                      "dup: it names channel 0 as the other carrier of that tone");
            }

            // --- sum a -20 dB copy of channel 0 into channel 5 ---
            {
                auto ch = split(clean);
                const float g = (float)it::dbToLin(-20.0);
                for (size_t i = 0; i < ch[5].size(); ++i) ch[5][i] += g * ch[0][i];
                const it::RoutingReport r = it::detectRouting(join(ch, sr), plan);
                check(!r.ok, "E-135.4e: bleed buffer yields ok=false at the 40 dB default");
                check(r.perChannel[5].verdict == it::Verdict::Bleed,
                      std::string("bleed: the contaminated channel reports 'bleed' (got ") +
                          vn(r.perChannel[5].verdict) + ")");
                check(r.perChannel[5].pairedWith == 0, "bleed: it names channel 0 as the interferer");
                const double m = r.perChannel[5].marginDb;
                check(std::abs(m - 20.0) <= 1.0,
                      "bleed: measured margin is 20 dB +/- 1 (got " + std::to_string(m) + " dB)");
                // and the SAME buffer passes when the caller asks for a looser gate — the
                // threshold is the caller's, not baked in.
                const it::RoutingReport loose = it::detectRouting(join(ch, sr), plan, 10.0);
                check(loose.ok, "bleed: the same buffer passes at minMarginDb=10 (gate is the "
                                "caller's threshold, applied to a measured number)");
            }
        }
    }

    // ==== structural refusals ================================================================
    {
        const it::Plan p6 = it::makePlan(6);
        meter::AudioBuffer b;
        b.channels = 4;
        b.sampleRate = 48000.0;
        b.frames = 100;
        b.samples.assign(400, 0.0f);
        const it::RoutingReport r = it::detectRouting(b, p6);
        check(!r.error.empty() && !r.ok, "width mismatch is a structural refusal, not a verdict");

        meter::AudioBuffer empty;
        const it::RoutingReport r2 = it::detectRouting(empty, p6);
        check(!r2.error.empty() && !r2.ok, "empty buffer is a structural refusal");
    }

    // ==== the plan constants are the corpus's, and the cap is honoured ========================
    {
        check(std::abs(it::toneHz(0) - 313.0) < 1e-9 && std::abs(it::toneHz(1) - 452.0) < 1e-9 &&
                  std::abs(it::toneHz(23) - 3510.0) < 1e-9,
              "tone plan: 313 / 452 / ... / 3510 Hz at k = 0 / 1 / 23");
        check(std::abs(it::kLfeHz - 40.0) < 1e-9, "LFE tone pinned at 40 Hz");
        check(std::abs(it::kDefaultLevelDb + 18.0) < 1e-9, "default level pinned at -18 dBFS");
        const it::Plan big = it::makePlan(500);
        check(big.channels() == it::kMaxSlots, "slot count clamps at the 128-channel REAPER cap");
        check(it::toneHz(127) < 24000.0, "the widest legal slot still fits below Nyquist @48k");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d IDENTITY-TONES UNIT TEST FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nALL IDENTITY-TONES UNIT TESTS PASSED\n");
    return 0;
}
