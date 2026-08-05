// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// position_params.h — representation-aware positional-param discovery + angle composition
// (D3). The SDK-free core behind spatial.set_source_position's param
// discovery, unit-tested without REAPER (unit.position_params).
//
// Why this exists: the tool used to discover axis params by bare-letter name scan ("x" as a
// substring of the param name). On an IEM encoder that scan lands on "Quaternion X" — a
// representation the ADM exporter never reads (findPositionalFx samples azimuth/elevation) —
// so a position "set" succeeded while the exported trajectory never moved (live-observed). Two rules
// fix the class:
//
//   1. Az/el-first preference: a panner that exposes azimuth/elevation params (IEM/SPARTA/
//      JSFX encoders) is driven in THAT representation, derived from the cartesian request
//      when needed — the same preference order findPositionalFx applies on export, so the
//      authoring write and the exported sample land on the same params.
//   2. The cartesian letter scan (ReaSurroundPan "in 1 X") never matches a quaternion
//      component: quaternion params are not positions and half-writes corrupt the rotation.
//
// Conventions (the tool's own, LOCKED): user space x=+right, y=+front, z=+up in [-1,1];
// azimuth degrees +left/CCW (ADM/IEM suite convention); elevation degrees +up. The forward
// map is x = -sin(az)cos(el), y = cos(az)cos(el), z = sin(el); composeTarget() applies its
// exact inverses (az = atan2(-x, y), el = asin(z)).

#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace posparam {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegPerRad = 180.0 / kPi;

// A param name (lowercased) that names a quaternion component. Matched on "quat" so
// abbreviated spellings ("Quat X") are caught too.
inline bool isQuaternionName(const std::string& lowerName) {
    return lowerName.find("quat") != std::string::npos;
}

// What a panner exposes, by param index (-1 = absent). Angle params win the representation
// choice: azElFirst() mirrors findPositionalFx's export-side scan (azim/elev presence).
struct Discovery {
    int azP = -1, elP = -1, distP = -1;  // angle representation ("azim"/"elev"/"dist")
    int xP = -1, yP = -1, zP = -1;       // cartesian representation (letter scan)
    bool skippedQuaternion = false;      // a letter scan passed over a quaternion component
    bool azElFirst() const { return azP >= 0; }
};

// Substring search with set_source_position's STRICT source-token semantics, preserved
// exactly from the pre-D3 scan: `source` is 0-based; the 1-based token (source+1) is always
// preferred; with source == 0 the first match is the fallback; with source > 0 a numbered
// match is REQUIRED (no token match -> -1), so a multi-source request never silently drives
// source 1. `excludeQuat` additionally skips quaternion-named params (the letter scan's D3
// hardening; angle keys can't collide with "quat" but route through one scanner for one
// set of semantics).
inline int findParam(const std::vector<std::string>& lowerNames, const std::string& key,
                     int source, bool excludeQuat, bool* skippedQuat = nullptr) {
    const std::string tok = std::to_string(source + 1);
    int fallback = -1;
    for (int p = 0; p < (int)lowerNames.size(); ++p) {
        const std::string& nm = lowerNames[p];
        if (nm.find(key) == std::string::npos) continue;
        if (excludeQuat && isQuaternionName(nm)) {
            if (skippedQuat) *skippedQuat = true;
            continue;
        }
        if (source == 0 && fallback < 0) fallback = p;
        if (nm.find(tok) != std::string::npos) return p;
    }
    return fallback;
}

// Classify a panner's params. `lowerNames` = the FX's param names, lowercased, in
// param-index order; `source` = the tool's 0-based multi-source index.
//
// The letter scan runs ONLY when no angle representation exists: single-letter substrings
// collide with angle-param names themselves ('z' ⊂ "azimuth", 'y' ⊂ "quality"), so on an
// angle roster the cartesian indices would be garbage — the unit test shows the pre-D3
// scan driving "Azimuth Angle" as z on the IEM StereoEncoder. Representation choice is
// all-or-nothing by construction.
inline Discovery discover(const std::vector<std::string>& lowerNames, int source) {
    Discovery d;
    d.azP = findParam(lowerNames, "azim", source, false);
    d.elP = findParam(lowerNames, "elev", source, false);
    d.distP = findParam(lowerNames, "dist", source, false);
    if (d.azP < 0) {
        d.xP = findParam(lowerNames, "x", source, true, &d.skippedQuaternion);
        d.yP = findParam(lowerNames, "y", source, true, &d.skippedQuaternion);
        d.zP = findParam(lowerNames, "z", source, true, &d.skippedQuaternion);
    }
    return d;
}

// The target angles a (possibly partial) request implies, in angle space.
struct TargetAngles {
    bool writeAz = false, writeEl = false;
    double azDeg = 0.0, elDeg = 0.0;
};

// Compose target azimuth/elevation (degrees) from the request. Direct angle args win
// verbatim. A cartesian request derives azimuth from atan2(-x, y) and elevation from
// asin(clamp(z)) — the forward map's exact inverses — and only REQUESTED components are
// written: z alone never recenters azimuth, x/y alone never flattens elevation. When
// exactly one of x/y is given, the missing horizontal member comes from the CURRENT
// azimuth's unit direction (curAzDeg), so a single-axis nudge keeps its meaning on an
// angle panner.
inline TargetAngles composeTarget(bool azArg, double azArgDeg, bool elArg, double elArgDeg,
                                  bool hasX, double x, bool hasY, double y,
                                  bool hasZ, double z, double curAzDeg) {
    TargetAngles r;
    if (azArg) {
        r.writeAz = true;
        r.azDeg = azArgDeg;
    } else if (hasX || hasY) {
        const double curRad = curAzDeg / kDegPerRad;
        const double xe = hasX ? x : -std::sin(curRad);
        const double ye = hasY ? y : std::cos(curRad);
        r.writeAz = true;
        r.azDeg = std::atan2(-xe, ye) * kDegPerRad;
    }
    if (elArg) {
        r.writeEl = true;
        r.elDeg = elArgDeg;
    } else if (hasZ) {
        const double zc = z < -1.0 ? -1.0 : (z > 1.0 ? 1.0 : z);
        r.writeEl = true;
        r.elDeg = std::asin(zc) * kDegPerRad;
    }
    return r;
}

}  // namespace posparam
}  // namespace reaper_mcp
