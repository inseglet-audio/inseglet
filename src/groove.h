// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// groove.h — SDK-free MIDI groove / quantize math (Batch B2).
//
// Pure functions over positions expressed in beats (quarter notes relative to the item start) and
// 0..127 velocities. No REAPER, no SDK — so every transform is unit-tested deterministically in
// tests/unit/test_groove.cpp and reused verbatim by the midi.* handlers in tools_midi.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace reaper_mcp {
namespace groove {

// Deterministic PRNG (xorshift64*) so midi.humanize is reproducible given a seed.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        uint64_t x = s;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    double unit() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }  // [0,1)
    double sym()  { return unit() * 2.0 - 1.0; }                                   // [-1,1)
};

// Clamp an int into [lo,hi].
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Snap a position (beats) to a grid at spacing `grid`, moved by `strength` (0..1), with `swing`
// (0..1) pushing odd grid slots later by swing*grid. strength 1 = full snap, 0 = unchanged.
inline double quantizePos(double beat, double grid, double strength, double swing) {
    if (grid <= 0.0 || strength <= 0.0) return beat;
    long idx = (long)std::llround(beat / grid);
    double target = (double)idx * grid;
    if (swing > 0.0 && (idx % 2 != 0)) target += swing * grid;
    if (strength > 1.0) strength = 1.0;
    return beat + strength * (target - beat);
}

// Velocity scaling around a center: v' = center + (v-center)*scale + offset, clamped to [lo,hi].
inline int scaleVelocity(int v, double scale, double offset, int center, int lo, int hi) {
    double out = (double)center + ((double)v - (double)center) * scale + offset;
    return clampi((int)std::lround(out), lo, hi);
}

// Transpose a pitch by semitones; returns false (leaving out unset) if it would leave 0..127.
inline bool transposePitch(int pitch, int semis, int& out) {
    int p = pitch + semis;
    if (p < 0 || p > 127) return false;
    out = p;
    return true;
}

// Time-scale a position around an anchor: anchor + (beat-anchor)*factor.
inline double stretchPos(double beat, double anchor, double factor) {
    return anchor + (beat - anchor) * factor;
}

// Randomize timing / velocity for humanize (deterministic given the shared Rng).
inline double humanizeTiming(double beat, double amountBeats, Rng& rng) {
    return beat + rng.sym() * amountBeats;
}
inline int humanizeVelocity(int vel, int amount, Rng& rng, int lo, int hi) {
    return clampi(vel + (int)std::lround(rng.sym() * (double)amount), lo, hi);
}

// One groove-template slot: a timing push (beats) and a velocity multiplier for that grid position.
struct GrooveStep {
    double timeOffset = 0.0;
    double velScale = 1.0;
};

// Grid slot index for a beat given a template of nSteps slots at `grid` spacing (wraps, non-negative).
inline int grooveSlot(double beat, double grid, int nSteps) {
    if (grid <= 0.0 || nSteps <= 0) return 0;
    long idx = (long)std::llround(beat / grid);
    long m = idx % (long)nSteps;
    if (m < 0) m += nSteps;
    return (int)m;
}

// Apply a groove template to a position + velocity at `strength` (0..1).
inline void applyGroove(double beat, int vel, double grid, const std::vector<GrooveStep>& tmpl,
                        double strength, double& outBeat, int& outVel) {
    if (tmpl.empty() || grid <= 0.0) { outBeat = beat; outVel = vel; return; }
    strength = strength < 0.0 ? 0.0 : (strength > 1.0 ? 1.0 : strength);
    const GrooveStep& g = tmpl[(size_t)grooveSlot(beat, grid, (int)tmpl.size())];
    outBeat = beat + strength * g.timeOffset;
    double vscale = 1.0 + strength * (g.velScale - 1.0);
    outVel = clampi((int)std::lround((double)vel * vscale), 1, 127);
}

// Extract an nSteps-slot groove template (at `grid`) from reference (beat, velocity) pairs:
// per-slot mean timing deviation from the grid line and mean velocity ratio vs `refVel`.
inline std::vector<GrooveStep> extractGroove(const std::vector<std::pair<double, int>>& notes,
                                             double grid, int nSteps, int refVel = 96) {
    std::vector<GrooveStep> t((size_t)std::max(1, nSteps));
    std::vector<int> cnt(t.size(), 0);
    std::vector<double> sumT(t.size(), 0.0), sumV(t.size(), 0.0);
    for (const auto& n : notes) {
        int slot = grooveSlot(n.first, grid, (int)t.size());
        long idx = (long)std::llround(n.first / grid);
        sumT[slot] += n.first - (double)idx * grid;
        sumV[slot] += (refVel > 0) ? (double)n.second / (double)refVel : 1.0;
        cnt[slot]++;
    }
    for (size_t i = 0; i < t.size(); ++i)
        if (cnt[i] > 0) { t[i].timeOffset = sumT[i] / cnt[i]; t[i].velScale = sumV[i] / cnt[i]; }
    return t;
}

// Legato: given time-sorted (startBeat, lengthBeat) notes, return new lengths so each note reaches the
// next note's start minus `gap` (floored at minLen); the last note keeps its length.
inline std::vector<double> legatoLengths(const std::vector<std::pair<double, double>>& notes,
                                         double gap, double minLen) {
    std::vector<double> out(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i + 1 < notes.size()) {
            double newLen = notes[i + 1].first - gap - notes[i].first;
            out[i] = newLen < minLen ? minLen : newLen;
        } else {
            out[i] = notes[i].second;
        }
    }
    return out;
}

}  // namespace groove
}  // namespace reaper_mcp
