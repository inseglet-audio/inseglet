#pragma once
// ================================================================================================
// identity_tones.h — B4 (doc 131 Batch C): spectral channel-identity QC, SDK-free.
//
// The method is the iamf-adm-corpus's WP1 spectral-identity convention, ported into the DAW:
// every channel/object carries a UNIQUE non-harmonic identifier sine, so any downstream routing
// loss, duplication, swap or bleed is detectable by looking for each tone where it should be.
//
//   f_k = 313 + 139*k Hz   ... iamf-adm-corpus/README.md:27 ("identifier sine (313 + 139*k Hz;
//   LFE at 40 Hz)"); doc 16 (WP3 ADM fidelity report) records the same convention for the 34-file
//   corpus. Level -18 dBFS, 2 s at 48 kHz = the figures doc 113 section 2 used for the insg_*
//   cross-validation fixtures, and the same plan doc 116 (E-116.1c / E-116.2) verified the Loom
//   channel-order bridge with.
//
// Why Goertzel and not an FFT: the tone set is KNOWN and DISCRETE, so a per-frequency Goertzel is
// exact in O(N) per tone with no window/leakage bookkeeping. At 48 kHz every 313 + 139*k completes
// an INTEGER number of cycles in any whole-second window, which is why the measured off-diagonal
// rejection in doc 116 was >= 60 dB. A caller who supplies a non-integer-cycle window raises
// leakage, so this header NEVER asserts identity from the arg-max alone: every verdict carries the
// measured marginDb, and the caller's threshold is applied to that number.
//
// NB tests/unit/test_loom_manifest.cpp deliberately keeps its OWN private goertzel() and its own
// RIFF reader. That is not duplication to be tidied away: it is an INDEPENDENT second witness that
// verifies the Loom WAV writer without using the writer's own code. Pointing it at this header
// would turn a real check into a tautology. Leave it alone.
//
// SDK-free by construction: no REAPER symbols. Detection consumes a meter::AudioBuffer, so the
// exact same code path serves an external WAV (meter::readWavFile), a rendered stem, and a
// render-free track audio-accessor read (audio_accessor.h) — the three surfaces doc 107's B4 names.
// ================================================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "ambisonic_meter.h"

namespace reaper_mcp {
namespace idtone {

// ---- The corpus plan, as constants, cited above. Never re-derived anywhere else. ---------------
inline constexpr double kBaseHz = 313.0;   // tone 0
inline constexpr double kStepHz = 139.0;   // per-slot step (non-harmonic against kBaseHz)
inline constexpr double kLfeHz = 40.0;     // LFE slots sit below the band, per the corpus rule
inline constexpr double kDefaultLevelDb = -18.0;
inline constexpr double kDefaultDurSec = 2.0;
inline constexpr int kDefaultRate = 48000;
// Default gate: deliberately BELOW the >= 60 dB the method measures, so the threshold is not
// tuned to its own observation.
inline constexpr double kDefaultMinMarginDb = 40.0;
inline constexpr double kDefaultSilenceDb = -80.0;
// A channel whose strongest PLAN tone accounts for less than this fraction of the channel's own
// RMS is not carrying a plan tone at all — it carries something the plan does not describe. Without
// this guard the arg-max over a set of near-zero powers is noise-driven, and the verdict label
// (swapped / duplicated) would be arbitrary even though ok=false is correct. Found by the E-135.2
// negative control, which feeds a 40 Hz LFE channel to a plan that has no 40 Hz slot; recorded in
// doc 135 as a post-hoc refinement of the classifier, not of the pre-registered vocabulary.
inline constexpr double kPlanCoverageFloor = 0.1;   // -20 dB
// Slot cap: REAPER tracks top out at 128 channels; 313 + 139*127 = 17966 Hz is still well inside
// the 48 kHz band, so the plan is representable for every legal width.
inline constexpr int kMaxSlots = 128;

inline double toneHz(int k) { return kBaseHz + kStepHz * (double)k; }

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

// ---- Plan ------------------------------------------------------------------------------------
struct Slot {
    int index = 0;              // 0-based slot (channel or track position)
    double hz = 0.0;            // the tone this slot must carry
    bool lfe = false;           // true when the 40 Hz LFE rule applied
    std::string label;          // optional speaker/track label, echoed in reports
};

struct Plan {
    int rate = kDefaultRate;
    double durSec = kDefaultDurSec;
    double levelDb = kDefaultLevelDb;
    bool lfeRule = true;        // false = pure 313+139k on every slot (the E-135.2 negative control)
    std::vector<Slot> slots;

    size_t frames() const {
        const double f = durSec * (double)rate;
        return f <= 0.0 ? 0u : (size_t)(f + 0.5);
    }
    int channels() const { return (int)slots.size(); }
};

// Build the plan for `n` slots. `lfeIdx` lists 0-based LFE slots (empty for object/track mode).
// `labels` is optional and may be shorter than n.
inline Plan makePlan(int n, const std::vector<int>& lfeIdx = {},
                     const std::vector<std::string>& labels = {}, int rate = kDefaultRate,
                     double durSec = kDefaultDurSec, double levelDb = kDefaultLevelDb,
                     bool lfeRule = true) {
    Plan p;
    p.rate = rate > 0 ? rate : kDefaultRate;
    p.durSec = durSec > 0.0 ? durSec : kDefaultDurSec;
    p.levelDb = levelDb;
    p.lfeRule = lfeRule;
    if (n < 0) n = 0;
    if (n > kMaxSlots) n = kMaxSlots;
    p.slots.reserve((size_t)n);
    for (int k = 0; k < n; ++k) {
        Slot s;
        s.index = k;
        s.lfe = lfeRule && (std::find(lfeIdx.begin(), lfeIdx.end(), k) != lfeIdx.end());
        s.hz = s.lfe ? kLfeHz : toneHz(k);
        if ((size_t)k < labels.size()) s.label = labels[(size_t)k];
        p.slots.push_back(s);
    }
    return p;
}

// Per-slot float buffers, one buffer per slot. Deterministic: no dither, no randomness, phase 0.
inline std::vector<std::vector<float>> renderPlan(const Plan& p) {
    const size_t N = p.frames();
    const double amp = dbToLin(p.levelDb);
    const double sr = (double)p.rate;
    std::vector<std::vector<float>> out((size_t)p.channels());
    for (size_t k = 0; k < out.size(); ++k) {
        const double w = 2.0 * M_PI * p.slots[k].hz / sr;
        out[k].resize(N);
        for (size_t i = 0; i < N; ++i) out[k][i] = (float)(amp * std::sin(w * (double)i));
    }
    return out;
}

// ---- Goertzel --------------------------------------------------------------------------------
// Power of frequency f (Hz) in channel `ch` of an interleaved buffer, over [begin, end) frames.
inline double goertzelPower(const meter::AudioBuffer& buf, int ch, double f, size_t begin,
                            size_t end) {
    if (ch < 0 || ch >= buf.channels || buf.sampleRate <= 0.0) return 0.0;
    if (end > buf.frames) end = buf.frames;
    if (begin >= end) return 0.0;
    const double w = 2.0 * M_PI * f / buf.sampleRate;
    const double c = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = begin; i < end; ++i) {
        s0 = (double)buf.at(i, ch) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return p > 0.0 ? p : 0.0;
}

inline double goertzelPower(const meter::AudioBuffer& buf, int ch, double f) {
    return goertzelPower(buf, ch, f, 0, buf.frames);
}

inline double channelRms(const meter::AudioBuffer& buf, int ch, size_t begin, size_t end) {
    if (ch < 0 || ch >= buf.channels) return 0.0;
    if (end > buf.frames) end = buf.frames;
    if (begin >= end) return 0.0;
    double acc = 0.0;
    for (size_t i = begin; i < end; ++i) {
        const double v = (double)buf.at(i, ch);
        acc += v * v;
    }
    return std::sqrt(acc / (double)(end - begin));
}

// ---- Verdicts --------------------------------------------------------------------------------
// Fixed vocabulary, declared in preregistration-135 section 2 (D7) BEFORE any output existed.
enum class Verdict { Identity, Swapped, Duplicated, Dropped, Bleed, Silent };

inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Identity:   return "identity";
        case Verdict::Swapped:    return "swapped";
        case Verdict::Duplicated: return "duplicated";
        case Verdict::Dropped:    return "dropped";
        case Verdict::Bleed:      return "bleed";
        case Verdict::Silent:     return "silent";
    }
    return "unknown";
}

struct ChannelReport {
    int channel = 0;
    std::string label;
    double expectHz = 0.0;
    int detectedSlot = -1;      // slot whose tone is strongest here (-1 = silent / undetermined)
    double detectedHz = 0.0;
    double marginDb = 0.0;      // own tone vs the strongest OTHER tone, power domain
    double rmsDb = meter::kMinDb();
    double planCoverage = 0.0;  // strongest plan tone's RMS / the channel's RMS (1.0 = pure tone)
    Verdict verdict = Verdict::Identity;
    int pairedWith = -1;        // swap partner / duplicate source / dominant interferer
    std::string note;
};

struct RoutingReport {
    bool ok = false;
    int channels = 0;
    int identityCount = 0;
    double worstMarginDb = 0.0;
    double minMarginDb = kDefaultMinMarginDb;
    std::vector<ChannelReport> perChannel;
    std::vector<std::string> findings;   // one human-readable line per non-identity channel
    std::string summary;
    std::string error;                   // set only on a structural refusal (width mismatch, etc.)
};

// Compare a buffer against a plan and report the detected routing map.
//
// Algorithm (fixed before any result existed):
//   detected[c] = argmax over slots j of goertzel(c, slot_j.hz)
//   silent(c)                                   -> Silent   (its expected tone is missing here)
//   planCoverage(c) < kPlanCoverageFloor         -> Dropped  (the plan does not explain this channel)
//   detected[c] == c and margin >= minMarginDb   -> Identity
//   detected[c] == c and margin <  minMarginDb   -> Bleed    (pairedWith = strongest interferer)
//   detected[detected[c]] == c                   -> Swapped  (pairedWith = detected[c])
//   some other channel d also peaks on detected[c] -> Duplicated (pairedWith = d)
//   otherwise                                    -> Dropped  (expected tone is not in this channel)
inline RoutingReport detectRouting(const meter::AudioBuffer& buf, const Plan& plan,
                                   double minMarginDb = kDefaultMinMarginDb,
                                   double silenceDb = kDefaultSilenceDb, size_t begin = 0,
                                   size_t end = (size_t)-1) {
    RoutingReport r;
    r.minMarginDb = minMarginDb;
    const int n = plan.channels();
    if (n <= 0) {
        r.error = "the plan has no slots";
        return r;
    }
    if (buf.channels != n) {
        r.error = "buffer has " + std::to_string(buf.channels) + " channels but the plan declares " +
                  std::to_string(n);
        return r;
    }
    if (buf.frames == 0 || buf.sampleRate <= 0.0) {
        r.error = "buffer has no frames or no sample rate";
        return r;
    }
    if (end > buf.frames) end = buf.frames;
    if (begin >= end) {
        r.error = "empty analysis window";
        return r;
    }

    r.channels = n;

    // Power matrix: p[c][j] = energy of slot j's tone found in channel c.
    std::vector<std::vector<double>> p((size_t)n, std::vector<double>((size_t)n, 0.0));
    std::vector<double> rms((size_t)n, 0.0);
    for (int c = 0; c < n; ++c) {
        rms[(size_t)c] = channelRms(buf, c, begin, end);
        for (int j = 0; j < n; ++j)
            p[(size_t)c][(size_t)j] = goertzelPower(buf, c, plan.slots[(size_t)j].hz, begin, end);
    }

    // Pass 1 — arg-max and silence.
    std::vector<int> detected((size_t)n, -1);
    for (int c = 0; c < n; ++c) {
        const double rdb = meter::linToDb(rms[(size_t)c]);
        if (rdb <= silenceDb) continue;   // leave detected = -1
        int best = 0;
        double bestP = p[(size_t)c][0];
        for (int j = 1; j < n; ++j)
            if (p[(size_t)c][(size_t)j] > bestP) { bestP = p[(size_t)c][(size_t)j]; best = j; }
        detected[(size_t)c] = best;
    }

    // Pass 2 — classify.
    r.worstMarginDb = 1e300;
    for (int c = 0; c < n; ++c) {
        ChannelReport cr;
        cr.channel = c;
        cr.label = plan.slots[(size_t)c].label;
        cr.expectHz = plan.slots[(size_t)c].hz;
        cr.rmsDb = meter::linToDb(rms[(size_t)c]);
        cr.detectedSlot = detected[(size_t)c];
        cr.detectedHz = cr.detectedSlot >= 0 ? plan.slots[(size_t)cr.detectedSlot].hz : 0.0;

        // Own vs strongest other, always measured, always reported.
        const double own = p[(size_t)c][(size_t)c];
        double worstOther = 0.0;
        int worstOtherIdx = -1;
        for (int j = 0; j < n; ++j) {
            if (j == c) continue;
            if (p[(size_t)c][(size_t)j] > worstOther) {
                worstOther = p[(size_t)c][(size_t)j];
                worstOtherIdx = j;
            }
        }
        cr.marginDb = 10.0 * std::log10((own > 0.0 ? own : 1e-300) /
                                        (worstOther > 0.0 ? worstOther : 1e-300));

        // How much of this channel actually IS the plan tone it peaks on. For x[i] = A sin(w i) at
        // an exact bin the Goertzel power is (A*N/2)^2, so the tone's RMS is sqrt(2P)/N; 1.0 means
        // the channel is that tone and nothing else.
        if (cr.detectedSlot >= 0 && rms[(size_t)c] > 0.0) {
            const double N = (double)(end - begin);
            const double bestP = p[(size_t)c][(size_t)cr.detectedSlot];
            const double toneRms = std::sqrt(2.0 * (bestP > 0.0 ? bestP : 0.0)) / N;
            cr.planCoverage = toneRms / rms[(size_t)c];
        }

        if (cr.detectedSlot < 0) {
            cr.verdict = Verdict::Silent;
            cr.note = "channel is silent (rms " + std::to_string(cr.rmsDb) +
                      " dB); its expected tone is not present";
        } else if (cr.planCoverage < kPlanCoverageFloor) {
            // The arg-max here is noise. Say so, rather than guessing swapped/duplicated from it.
            cr.verdict = Verdict::Dropped;
            cr.pairedWith = -1;
            cr.detectedSlot = -1;
            cr.detectedHz = 0.0;
            cr.note = "expected tone absent and the channel's content is not in the plan at all "
                      "(strongest plan tone covers only " + std::to_string(cr.planCoverage) +
                      " of its RMS)";
        } else if (cr.detectedSlot == c) {
            if (cr.marginDb >= minMarginDb) {
                cr.verdict = Verdict::Identity;
            } else {
                cr.verdict = Verdict::Bleed;
                cr.pairedWith = worstOtherIdx;
                cr.note = "carries its own tone but slot " + std::to_string(worstOtherIdx) +
                          "'s tone is only " + std::to_string(cr.marginDb) + " dB below it";
            }
        } else {
            const int d = cr.detectedSlot;
            if (detected[(size_t)d] == c) {
                cr.verdict = Verdict::Swapped;
                cr.pairedWith = d;
                cr.note = "carries slot " + std::to_string(d) + "'s tone while slot " +
                          std::to_string(d) + " carries this one";
            } else {
                int dup = -1;
                for (int o = 0; o < n; ++o)
                    if (o != c && detected[(size_t)o] == d) { dup = o; break; }
                if (dup >= 0) {
                    cr.verdict = Verdict::Duplicated;
                    cr.pairedWith = dup;
                    cr.note = "carries slot " + std::to_string(d) +
                              "'s tone, which also appears on channel " + std::to_string(dup);
                } else {
                    cr.verdict = Verdict::Dropped;
                    cr.pairedWith = d;
                    cr.note = "expected tone absent; the dominant tone here belongs to slot " +
                              std::to_string(d);
                }
            }
        }

        if (cr.verdict == Verdict::Identity) {
            ++r.identityCount;
            if (cr.marginDb < r.worstMarginDb) r.worstMarginDb = cr.marginDb;
        } else {
            std::string lbl = cr.label.empty() ? ("ch " + std::to_string(c)) : cr.label;
            r.findings.push_back(lbl + ": " + verdictName(cr.verdict) +
                                 (cr.note.empty() ? "" : " — " + cr.note));
        }
        r.perChannel.push_back(cr);
    }
    if (r.worstMarginDb > 1e299) r.worstMarginDb = 0.0;   // nothing was identity

    r.ok = (r.identityCount == n) && r.findings.empty() && (r.worstMarginDb >= minMarginDb);
    r.summary = r.ok ? ("identity mapping on all " + std::to_string(n) +
                        " channels (worst margin " + std::to_string((int)r.worstMarginDb) + " dB)")
                     : (std::to_string(n - r.identityCount) + " of " + std::to_string(n) +
                        " channels do not carry their own tone");
    return r;
}

// Convenience: the declared-vs-detected map, 0-based, -1 where undetermined.
inline std::vector<int> detectedMap(const RoutingReport& r) {
    std::vector<int> m;
    m.reserve(r.perChannel.size());
    for (const auto& c : r.perChannel) m.push_back(c.detectedSlot);
    return m;
}

}  // namespace idtone
}  // namespace reaper_mcp
