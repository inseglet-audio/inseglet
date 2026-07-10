// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// decode_timeline.h — SDK-free per-object decode-coverage timeline math (Batch T1).
//
// The object-model complement of analysis.decode_coverage (which decodes a whole ambisonic SCENE bus):
// given an OBJECT's position trajectory (a time-ordered sequence of az/el/gain samples — read from a
// panner's automation, or supplied as explicit keyframes), this header answers, at every time slice,
// "which delivery loudspeaker(s) does this object energize, and how does that migrate over time?".
//
// For each sample it ENCODES the object direction to real SN3D spherical harmonics at `order`, then
// PROJECTION-DECODES onto a set of nominal loudspeaker directions with the SAME basis analysis.
// decode_coverage uses (meter::realSHsn3d) — so a hard-left object lands on the left speakers, a
// height-panned object lands on the top layer, and a sweep migrates the dominant speaker crisply
// (sharper at higher order). Per object it then aggregates a time-weighted coverage footprint (via
// meter::computeCoverageStats — reused verbatim), the dominant-speaker migration path, per-speaker
// dwell fractions, and the great-circle angular travel.
//
// Conventions (kept byte-consistent with the rest of the suite; docs/CONVENTIONS.md §4):
//   * Object azimuth is +LEFT (CCW from front) — the ambisonic/IEM convention spatial.ambisonic_encode
//     and export_adm speak; elevation is +UP. dirFromAzEl() maps (az,el) -> the internal right-handed
//     meter::Dir frame (x=right, y=front, z=up), identical to adm::sphToCart with distance=1 (direction
//     is distance-invariant, so distance is carried for reporting only, never for the decode).
//   * Speaker directions are supplied by the caller already in the meter::Dir frame (built from the
//     analysis.decode_coverage namedLayoutDirs table), so no layout knowledge lives here.
//
// No REAPER, no SDK — every function is deterministic and unit-tested in tests/unit/test_decode_timeline.cpp,
// and reused verbatim by analysis.object_decode_timeline (tools_analysis.cpp).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "ambisonic_meter.h"   // meter::Dir, realSHsn3d, computeCoverageStats, vecToAzEl, rad2deg, clampd

namespace reaper_mcp {
namespace decode_timeline {

// One object position sample: time (programme seconds), spherical position (az +left deg, el +up deg,
// distance 0..1+ — informational), and a linear gain. A static object collapses to a single sample.
struct Sample {
    double t = 0.0;
    double azDeg = 0.0;
    double elDeg = 0.0;
    double dist = 1.0;
    double gain = 1.0;
};

// Object azimuth (+left/CCW deg) + elevation (+up deg) -> internal right-handed direction
// (x=right, y=front, z=up). Identical to adm::sphToCart(az,el,1): x=-sin(a)cos(e), y=cos(a)cos(e),
// z=sin(e). meter::vecToAzEl(dirFromAzEl(a,e)) round-trips (a,e). Distance-invariant (unit vector).
inline meter::Dir dirFromAzEl(double azDeg, double elDeg) {
    const double a = azDeg * M_PI / 180.0, e = elDeg * M_PI / 180.0;
    return meter::Dir{-std::sin(a) * std::cos(e), std::cos(a) * std::cos(e), std::sin(e)};
}

// (order+1)^2 ACN channels for a given ambisonic order.
inline int acnCount(int order) { return (order + 1) * (order + 1); }

// Precompute the per-speaker SN3D SH decode basis (rows = speakers, cols = acnCount(order) channels),
// mirroring meter::decodeSpeakerEnergies' ambiX-axis mapping (X=front, Y=left, Z=up). Built once per
// (layout, order); reused for every object and every sample.
inline std::vector<std::vector<double>> speakerBasis(const std::vector<meter::Dir>& speakers, int order) {
    const int acn = acnCount(order < 1 ? 1 : order);
    std::vector<std::vector<double>> basis(speakers.size(), std::vector<double>(acn, 0.0));
    for (std::size_t s = 0; s < speakers.size(); ++s) {
        const double axf = speakers[s].y, ayl = -speakers[s].x, azu = speakers[s].z;  // -> ambiX axes
        const double azAmbi = std::atan2(ayl, axf);
        const double elAmbi = std::atan2(azu, std::hypot(axf, ayl));
        for (int k = 0; k < acn; ++k) basis[s][k] = meter::realSHsn3d(k, azAmbi, elAmbi);
    }
    return basis;
}

// Per-speaker decoded ENERGY of a single object direction: encode the direction to SN3D SH (order),
// project onto each speaker's precomputed basis, square, scale by gain^2. E_s peaks on the speaker
// nearest the object direction and falls off with angle (sharper at higher order) — the SH addition
// theorem's P_l(cos gamma) kernel. `basis` must have been built at the same `order`.
inline std::vector<double> objectSpeakerEnergies(const std::vector<std::vector<double>>& basis,
                                                 const meter::Dir& objDir, int order, double gain) {
    const int acn = acnCount(order < 1 ? 1 : order);
    // Encode the object direction (its own SH coefficients), in the ambiX frame.
    const double axf = objDir.y, ayl = -objDir.x, azu = objDir.z;
    const double azAmbi = std::atan2(ayl, axf);
    const double elAmbi = std::atan2(azu, std::hypot(axf, ayl));
    std::vector<double> a(acn, 0.0);
    for (int k = 0; k < acn; ++k) a[k] = meter::realSHsn3d(k, azAmbi, elAmbi);
    std::vector<double> E(basis.size(), 0.0);
    const double g2 = gain * gain;
    for (std::size_t s = 0; s < basis.size(); ++s) {
        double g = 0.0;
        const int n = (int)std::min(basis[s].size(), a.size());
        for (int k = 0; k < n; ++k) g += a[k] * basis[s][k];
        E[s] = g * g * g2;
    }
    return E;
}

// One resolved timeline slice: the sample time + position, the per-speaker energy FRACTIONS, and the
// dominant speaker (max energy) with its fraction.
struct Slice {
    double t = 0.0;
    double azDeg = 0.0, elDeg = 0.0, dist = 1.0;
    int dominantIdx = -1;
    double dominantFrac = 0.0;
    std::vector<double> frac;   // per-speaker energy fraction (sums to 1 when the slice carried energy)
};

// A dominant-speaker run in the migration path: speaker index, first/last sample time it was dominant.
struct MigrationRun {
    int speakerIdx = -1;
    double startT = 0.0, endT = 0.0;
};

// The full per-object result.
struct ObjectTimeline {
    std::vector<Slice> slices;               // one per input sample, in order
    std::vector<double> dwellFrac;           // per-speaker fraction of (time-weighted) DOMINANCE, sums<=1
    std::vector<MigrationRun> migration;     // run-length of the dominant speaker over time
    meter::CoverageStats footprint;          // computeCoverageStats over the time-weighted total energy
    int speakersTouched = 0;                 // distinct speakers that were ever dominant
    double angularTravelDeg = 0.0;           // summed great-circle angle between consecutive directions
    double maxElevationDeg = 0.0, minElevationDeg = 0.0;
    bool isStatic = true;                    // every sample within kStaticEps of the first
};

// Angular gap (degrees) between two unit directions.
inline double angleBetweenDeg(const meter::Dir& a, const meter::Dir& b) {
    const double dot = meter::clampd(a.x * b.x + a.y * b.y + a.z * b.z, -1.0, 1.0);
    return meter::rad2deg(std::acos(dot));
}

// Below this total angular travel (deg) an object is reported static.
inline double kStaticEpsDeg() { return 1.0; }

// Compute the decode-coverage timeline of one object over `samples` against `speakers` at `order`.
// `deadZoneFrac` is forwarded to the footprint's coverage stats. Time weighting: each sample owns the
// forward interval to the next sample (the last sample mirrors the previous interval; a lone sample
// weights 1.0), so dwell and the aggregate footprint are duration-proportional even for uneven grids.
inline ObjectTimeline computeTimeline(const std::vector<Sample>& samples,
                                      const std::vector<meter::Dir>& speakers, int order,
                                      double deadZoneFrac = 0.1) {
    ObjectTimeline out;
    if (order < 1) order = 1;
    const std::size_t ns = speakers.size();
    if (samples.empty() || ns == 0) return out;

    const std::vector<std::vector<double>> basis = speakerBasis(speakers, order);
    std::vector<double> totalE(ns, 0.0);        // time-weighted energy across all samples (footprint)
    std::vector<double> dwellW(ns, 0.0);        // time-weighted dominance weight per speaker
    double totalW = 0.0;

    // Per-sample weight = interval to the next sample; final/lone sample mirrors the previous interval.
    auto weightAt = [&](std::size_t i) -> double {
        if (samples.size() == 1) return 1.0;
        if (i + 1 < samples.size()) {
            const double dt = samples[i + 1].t - samples[i].t;
            return dt > 0.0 ? dt : 0.0;
        }
        const double dt = samples[i].t - samples[i - 1].t;   // last sample: reuse the prior interval
        return dt > 0.0 ? dt : 0.0;
    };
    // If every interval collapsed to zero (degenerate timestamps), fall back to equal weights.
    double probe = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) probe += weightAt(i);
    const bool equalWeights = (probe <= 0.0);

    out.maxElevationDeg = -1e9; out.minElevationDeg = 1e9;
    meter::Dir prevDir{0, 0, 0}; bool havePrev = false;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Sample& sm = samples[i];
        const meter::Dir d = dirFromAzEl(sm.azDeg, sm.elDeg);
        const double w = equalWeights ? 1.0 : weightAt(i);

        std::vector<double> E = objectSpeakerEnergies(basis, d, order, sm.gain);
        double eTot = 0.0; for (double e : E) eTot += e;

        Slice sl;
        sl.t = sm.t; sl.azDeg = sm.azDeg; sl.elDeg = sm.elDeg; sl.dist = sm.dist;
        sl.frac.assign(ns, 0.0);
        int dom = -1; double domE = -1.0;
        for (std::size_t s = 0; s < ns; ++s) {
            sl.frac[s] = (eTot > 1e-30) ? E[s] / eTot : 0.0;
            if (E[s] > domE) { domE = E[s]; dom = (int)s; }
            totalE[s] += w * E[s];
        }
        sl.dominantIdx = dom;
        sl.dominantFrac = (dom >= 0) ? sl.frac[(std::size_t)dom] : 0.0;
        if (dom >= 0) dwellW[(std::size_t)dom] += w;
        totalW += w;
        out.slices.push_back(std::move(sl));

        if (sm.elDeg > out.maxElevationDeg) out.maxElevationDeg = sm.elDeg;
        if (sm.elDeg < out.minElevationDeg) out.minElevationDeg = sm.elDeg;
        if (havePrev) out.angularTravelDeg += angleBetweenDeg(prevDir, d);
        prevDir = d; havePrev = true;
    }
    if (out.slices.empty()) return out;
    if (out.maxElevationDeg < -1e8) out.maxElevationDeg = 0.0;
    if (out.minElevationDeg > 1e8) out.minElevationDeg = 0.0;
    out.isStatic = out.angularTravelDeg < kStaticEpsDeg();

    // Dwell fractions (time-weighted dominance).
    out.dwellFrac.assign(ns, 0.0);
    if (totalW > 0.0) for (std::size_t s = 0; s < ns; ++s) out.dwellFrac[s] = dwellW[s] / totalW;

    // Migration: run-length-encode the dominant speaker over successive slices.
    for (const Slice& sl : out.slices) {
        if (sl.dominantIdx < 0) continue;
        if (out.migration.empty() || out.migration.back().speakerIdx != sl.dominantIdx) {
            MigrationRun r; r.speakerIdx = sl.dominantIdx; r.startT = sl.t; r.endT = sl.t;
            out.migration.push_back(r);
        } else {
            out.migration.back().endT = sl.t;
        }
    }
    // Distinct dominant speakers.
    std::vector<int> seen;
    for (const MigrationRun& r : out.migration)
        if (std::find(seen.begin(), seen.end(), r.speakerIdx) == seen.end()) seen.push_back(r.speakerIdx);
    out.speakersTouched = (int)seen.size();

    // Aggregate coverage footprint over the time-weighted total energy.
    out.footprint = meter::computeCoverageStats(speakers, totalE, deadZoneFrac);
    return out;
}

}  // namespace decode_timeline
}  // namespace reaper_mcp
